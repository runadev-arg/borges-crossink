#include "BorgesGlue.h"

#include <BookmarkStore.h>
#include <BuildScratch.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <InflateStream.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>
#include <ctime>
#include <optional>
#include <string>

#include "BorgesActivities.h"
#include "BorgesStack.h"
#include "BorgesUi.h"
#include "ChapterXPathResolver.h"
#include "ClippingStore.h"
#include "CrossPointState.h"
#include "ProgressMapper.h"
#include "ReaderTracker.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"

namespace borges {
namespace glue {

namespace {

constexpr const char* kCacheRoot = "/.crosspoint";
constexpr const char* kUndoFlag = "/borges_undo";
constexpr const char* kClipTimes = "/borges_clip_times.bin";
constexpr int64_t kPlausibleEpoch = 1600000000;
// A single resolve streams a whole chapter, so a book with many new clippings spreads
// them over a few exits instead of holding one exit for minutes.
constexpr int kMaxXpointersPerFinalize = 6;
constexpr uint32_t kXpointerBudgetMs = 4000;
constexpr uint32_t kPositionBudgetMs = 8000;
// With the framebuffer lent, inflate's 43 KB come out of it. What is left on the heap is the
// XML parser, its string pools and the read chunk.
constexpr uint32_t kMinFreeForXpointer = 22 * 1024;  // 18 KB peak measured on the host.
constexpr uint32_t kMinBlockForXpointer = 8 * 1024;
constexpr uint32_t kAllocationMargin = 2 * 1024;
// A chapter this size takes seconds to walk even stopping early: the screen says something first.
constexpr size_t kBigChapterBytes = 192 * 1024;

#if !defined(RTC_NOINIT_ATTR)
#define RTC_NOINIT_ATTR
#endif
// Survives the restart a screen leaves through, which has no time to write to the card with a
// session still holding it. Eight bytes of RTC memory, not DRAM, and read only when one is armed.
constexpr uint32_t kLeftOnPurposeMagic = 0x4C454654;  // "LEFT"
RTC_NOINIT_ATTR uint32_t sLeftOnPurpose;

bool takeLeftOnPurpose() {
  const bool left = sLeftOnPurpose == kLeftOnPurposeMagic;
  sLeftOnPurpose = 0;
  return left;
}

std::string cachePathFor(const std::string& path) { return Epub::cachePathForFilePath(path, kCacheRoot); }
std::string trackedPath(const Epub& epub) { return epub.getCachePath() + "/borges_reading.bin"; }

const char* fileNameOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path.c_str() : path.c_str() + slash + 1;
}

// Holds the book before anything can go wrong on the way to its check: no link, a press
// that skips the session, a crash in that boot. The hold is what keeps stale progress from
// quietly becoming the head at the next sleep.
void holdBook(BorgesStack& stack, const std::string& path) {
  // With the search off nothing will ever ask, and a hold would keep this place back for good.
  if (!stack.glue.autoOnOpen) return;
  BookId id;
  if (!partialMd5(stack.fs, path.c_str(), id) || !stack.engine.onBookOpened(id)) {
    LOG_ERR("BORGES", "Could not hold %s", path.c_str());
  }
}

OpenRoute routeBook(BorgesStack& stack, const std::string& path) {
  OpenInputs in;
  in.isEpub = FsHelpers::hasEpubExtension(path);
  in.hasPendingJump = APP_STATE.pendingBookmarkSpine != UINT16_MAX;
  in.paired = stack.store.creds().paired();
  if (in.paired) {
    const Gate gate = stack.engine.decideAutomatic(SessionKind::Open);
    in.needsRelink = gate == Gate::NeedsRelink;
    in.backedOff = openBackedOff(gate == Gate::LinkSkipped, gate == Gate::BackedOff, stack.glue, stack.clock.wallNow());
  }
  in.hasSavedWifi = hasSavedWifi();
  in.autoOnOpen = stack.glue.autoOnOpen;
  in.throttled = openThrottled(stack.glue, pathHash(path.c_str()), stack.clock.wallNow());
  in.batteryPercent = powerManager.getBatteryPercentage();
  OpenRoute route = routeOpen(in);
  if (route != OpenRoute::Direct) holdBook(stack, path);
  bool missed = routeMissesCheck(in);
  if (route == OpenRoute::NetworkSession) {
    if (!armSession(stack.glue, PendingKind::OpenBook, takeLeftOnPurpose())) {
      LOG_INF("BORGES", "Open sessions keep failing: opening without one");
      route = OpenRoute::DirectWithHold;
      missed = true;
    }
    stack.glue.lastOpenTryEpoch = stack.clock.wallNow().epochSec;
  }
  if (missed) noteCheckMissed(stack.glue, pathHash(path.c_str()));
  if (missed || route == OpenRoute::NetworkSession) stack.glueStore.save(stack.glue);
  return route;
}

// Paired or about to be: the previous firmware's credential is imported on first use.
bool mayBePaired() {
  return Storage.exists("/.crosspoint/borges/cred.a") || Storage.exists("/.crosspoint/borges/cred.b") ||
         Storage.exists(kLegacyCredentialPath) || Storage.exists(kLegacyCredentialPathOld);
}

// An xpointer means streaming a chapter through inflate (43 KB) on a heap that a loaded
// reader leaves at 35 KB, so it can only run on the framebuffer's storage. Lent on the first
// resolve of this finalize and returned once at the end: most exits resolve nothing and
// keep their frame. The panel keeps showing its image; the bytes in RAM do not survive.
class XpointerBudget {
 public:
  XpointerBudget(GfxRenderer& renderer, const Epub& epub) : renderer_(renderer), epub_(epub), startedAt_(millis()) {}
  ~XpointerBudget() { ChapterXPathResolver::setDeadline(0); }

  // False: not now. Whoever asked retries at a later finalize, or goes without. The clock runs for
  // the position too, which goes first and may take longer: a chapter of megabytes can spend it all,
  // and then the position leaves with its percentage and the clippings wait for an exit with time.
  bool take(const bool bounded, const int spine) {
    const uint32_t limitMs = bounded ? kXpointerBudgetMs : kPositionBudgetMs;
    if (millis() - startedAt_ >= limitMs) return false;
    if (bounded && taken_ >= kMaxXpointersPerFinalize) return false;
    ChapterXPathResolver::setDeadline(startedAt_ + limitMs);
    if (!loan_) {
      // Before the frame goes: after that there is nothing to draw on. Going to sleep the panel
      // already shows what it should.
      if (chapterBytes(spine) >= kBigChapterBytes && !activityManager.isEnteringSleep()) {
        GUI.drawPopup(renderer_, tr(STR_BORGES_ONE_MOMENT));
      }
      loan_ = makeUniqueNoThrow<GfxRenderer::FrameBufferLoan>(renderer_);
    }
    if (!loan_ || !buildscratch::available(InflateStream::requiredStorageSize(true)) ||
        ESP.getFreeHeap() < kMinFreeForXpointer || ESP.getMaxAllocHeap() < kMinBlockForXpointer) {
      LOG_ERR("BORGES", "No memory for an xpointer (free=%u maxAlloc=%u)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      return false;
    }
    ++taken_;
    return true;
  }
  bool frameLent() const { return loan_ != nullptr; }

 private:
  size_t chapterBytes(const int spine) const {
    const size_t upTo = epub_.getCumulativeSpineItemSize(spine);
    const size_t before = spine > 0 ? epub_.getCumulativeSpineItemSize(spine - 1) : 0;
    return upTo > before ? upTo - before : 0;
  }

  GfxRenderer& renderer_;
  const Epub& epub_;
  std::unique_ptr<GfxRenderer::FrameBufferLoan> loan_;
  uint32_t startedAt_;
  int taken_ = 0;
};

// What identifies each of the book's clippings. All an opening book needs to know about them.
class ClipKeys : public IClipSource {
 public:
  bool count(size_t& out) override {
    out = CLIPPINGS.clippingCount() + BOOKMARKS.getBookmarks().size();
    return true;
  }

  bool key(const size_t index, ClipKey& out) override {
    const size_t clips = CLIPPINGS.clippingCount();
    if (index < clips) {
      const Clipping* clip = CLIPPINGS.clippingAt(index);
      if (!clip) return false;
      out.spine = clip->spineIndex;
      out.textOffset = clip->paragraphIndex;
      out.textHash = clipIdentity(clip->startWordIndex, clip->textLength, clip->wordCount);
      out.bookmark = false;
      return true;
    }
    const auto& bookmarks = BOOKMARKS.getBookmarks();
    if (index - clips >= bookmarks.size()) return false;
    const Bookmark& mark = bookmarks[index - clips];
    out = bookmarkKey(mark.spineIndex, mark.paragraphIndex, mark.progress);
    return true;
  }

  bool detail(size_t, ClipDetail&) override { return false; }
};

class ClipSource final : public ClipKeys {
 public:
  ClipSource(BorgesStack& stack, std::shared_ptr<Epub> epub, XpointerBudget& budget)
      : stack_(stack), epub_(std::move(epub)), budget_(budget) {
    timesPath_ = epub_->getCachePath() + kClipTimes;
  }

  bool detail(const size_t index, ClipDetail& out) override {
    // The core treats false as "try again at the next finalize", and nothing is lost.
    const size_t clips = CLIPPINGS.clippingCount();
    uint16_t spine = 0;
    uint16_t paragraph = UINT16_MAX;
    uint32_t visibleOffset = kNoVisibleOffset;
    out = ClipDetail{};
    if (index < clips) {
      const Clipping* clip = CLIPPINGS.clippingAt(index);
      // std::string::resize aborts when it cannot grow.
      if (!clip || ESP.getMaxAllocHeap() < clip->textLength + kAllocationMargin ||
          !CLIPPINGS.readClippingText(index, text_)) {
        return false;
      }
      chapter_ = clip->chapterTitle;
      spine = clip->spineIndex;
      paragraph = clip->paragraphIndex;
      out.text = text_.c_str();
      // The core derives the book-wide page from this, as for progress. startPage is the
      // chapter's, and means nothing elsewhere.
      out.hasPct = true;
      out.pctMicro = static_cast<uint32_t>(
          epub_->calculateProgress(spine,
                                   clip->pageCount > 0 ? static_cast<float>(clip->startPage) / clip->pageCount : 0.0f) *
          100000000.0f);
      ClipStamp stamp;
      if (ClipTimes(stack_.fs).lookup(timesPath_.c_str(), spine, clip->timestamp, stamp)) {
        out.timeMs = stamp.epochMs;
        visibleOffset = stamp.visibleOffset;
      }
    } else {
      const auto& bookmarks = BOOKMARKS.getBookmarks();
      if (index - clips >= bookmarks.size()) return false;
      const Bookmark& mark = bookmarks[index - clips];
      chapter_ = mark.chapterTitle;
      spine = mark.spineIndex;
      paragraph = mark.paragraphIndex;
      text_ = mark.snippet[0] ? mark.snippet : mark.chapterTitle;
      out.text = text_.c_str();
      out.hasPct = true;
      out.pctMicro = static_cast<uint32_t>(epub_->calculateProgress(spine, mark.progress) * 100000000.0f);
      uint32_t progressBits = 0;
      memcpy(&progressBits, &mark.progress, sizeof(progressBits));
      ClipStamp stamp;
      if (ClipTimes(stack_.fs).lookup(timesPath_.c_str(), spine, progressBits, stamp)) {
        out.timeMs = stamp.epochMs;
        visibleOffset = stamp.visibleOffset;
      }
    }
    out.chapter = chapter_.c_str();
    xpointer_.clear();
    range_ = {};
    if (visibleOffset != kNoVisibleOffset || paragraph != UINT16_MAX || index < clips) {
      // Published without one it would stay without one: the ledger never asks again.
      if (!budget_.take(true, spine)) return false;
      // locateText builds a second copy of the text, and a std::string aborts when it cannot grow.
      if (index < clips && ESP.getMaxAllocHeap() < text_.size() + kAllocationMargin) return false;
      // A clipping reaches KOReader as a highlight only with both ends, holding the text it is
      // sent with. When its text cannot be found it goes on with no end: KOReader leaves it out
      // instead of showing other text, and the server still has it.
      if (index < clips &&
          ChapterXPathResolver::locateText(epub_, spine, text_.c_str(), visibleOffset,
                                           CLIPPINGS.clippingAt(index)->startWordIndex, range_) &&
          range_.pos0.size() < kMaxXpointer && range_.pos1.size() < kMaxXpointer) {
        out.xpointer = range_.pos0.c_str();
        out.xpointerEnd = range_.pos1.c_str();
        out.text = range_.text.c_str();
        return true;
      }
      // Time ran out, which says nothing about the text: this one waits, whole, for the next exit.
      if (ChapterXPathResolver::deadlineHit()) return false;
      if (visibleOffset != kNoVisibleOffset) {
        xpointer_ = ChapterXPathResolver::findXPathForVisibleTextOffset(epub_, spine, visibleOffset);
      } else if (paragraph != UINT16_MAX) {
        xpointer_ = ChapterXPathResolver::findXPathForParagraph(epub_, spine, paragraph);
      }
      if (ChapterXPathResolver::deadlineHit()) return false;
    }
    if (!xpointer_.empty() && xpointer_.size() < kMaxXpointer) out.xpointer = xpointer_.c_str();
    return true;
  }

 private:
  BorgesStack& stack_;
  std::shared_ptr<Epub> epub_;
  std::string timesPath_;
  std::string text_;
  std::string chapter_;
  std::string xpointer_;
  ChapterXPathResolver::TextRange range_;
  XpointerBudget& budget_;
};

}  // namespace

ClipKey bookmarkKey(const uint16_t spine, const uint16_t paragraph, const float progress) {
  uint32_t progressBits = 0;
  memcpy(&progressBits, &progress, sizeof(progressBits));
  ClipKey key;
  key.spine = spine;
  key.textOffset = paragraph;
  key.textHash = bookmarkIdentity(progressBits);
  key.bookmark = true;
  return key;
}

std::unique_ptr<Activity> interceptOpen(GfxRenderer& renderer, MappedInputManager& input, const std::string& path) {
  if (!FsHelpers::hasEpubExtension(path) || !mayBePaired()) return nullptr;
  auto stack = makeStack();
  if (!stack || !stack->openStore()) return nullptr;
  if (routeBook(*stack, path) != OpenRoute::NetworkSession) return nullptr;
  return makeUniqueNoThrow<BorgesHandoffActivity>(renderer, input, NetPayload::OpenBook, path);
}

bool wakeWantsNetworkBoot(const std::string& path) {
  if (!FsHelpers::hasEpubExtension(path) || !mayBePaired()) return false;
  auto stack = makeStack();
  if (!stack || !stack->openStore()) return false;
  return routeBook(*stack, path) == OpenRoute::NetworkSession;
}

void noteOpenedWithoutSession(const std::string& path) {
  if (!FsHelpers::hasEpubExtension(path) || !mayBePaired()) return;
  auto stack = makeStack();
  if (!stack || !stack->openStore() || !stack->store.creds().paired()) return;
  holdBook(*stack, path);
}

bool launchNetworkTarget(const uint32_t payload, GfxRenderer& renderer, MappedInputManager& input) {
  const auto what = static_cast<NetPayload>(payload);
  std::unique_ptr<Activity> activity;
  switch (what) {
    case NetPayload::OpenBook:
      if (APP_STATE.openEpubPath.empty()) return false;
      activity = makeUniqueNoThrow<BorgesOpenActivity>(renderer, input, APP_STATE.openEpubPath);
      break;
    case NetPayload::SleepFlush:
      activity = makeUniqueNoThrow<BorgesSleepFlushActivity>(renderer, input);
      break;
    case NetPayload::Library:
      activity = makeUniqueNoThrow<BorgesLibraryActivity>(renderer, input);
      break;
    case NetPayload::Status:
    case NetPayload::Pair:
    case NetPayload::ManualFromHome:
    case NetPayload::ManualFromReader:
    case NetPayload::Disconnect:
    case NetPayload::OtherDevice:
      activity = makeUniqueNoThrow<BorgesActivity>(renderer, input, true, what);
      break;
    case NetPayload::None:
      break;
  }
  if (!activity) {
    LOG_ERR("BORGES", "Could not start network target %u (free=%u)", static_cast<unsigned>(payload), ESP.getFreeHeap());
    return false;
  }
  activityManager.replaceActivity(std::move(activity));
  return true;
}

bool shouldFlushBeforeSleep(const uint32_t batteryPercent, const bool inNetworkBoot) {
  if (inNetworkBoot || !mayBePaired()) return false;
  auto stack = makeStack();
  if (!stack || !stack->openStore() || !stack->store.creds().paired() || !stack->openEngine()) return false;
  SleepInputs in;
  in.paired = true;
  in.syncOnSleep = stack->glue.syncOnSleep;
  in.hasSavedWifi = hasSavedWifi();
  in.gateGo = stack->engine.decideAutomatic(SessionKind::Sleep) == Gate::Go;
  in.batteryPercent = batteryPercent;
  if (!glue::shouldFlushBeforeSleep(in)) return false;
  const bool armed = armSession(stack->glue, PendingKind::SleepFlush, takeLeftOnPurpose());
  stack->glueStore.save(stack->glue);
  return armed;
}

bool finalizeFromReader(GfxRenderer& renderer, ReaderTracker& tracker, const std::shared_ptr<Epub>& epub,
                        const ReaderSnapshot& snapshot) {
  if (!epub || !mayBePaired()) return false;
  auto stack = makeStack();
  if (!stack || !stack->openStore() || !stack->store.creds().paired()) return false;
  BookId id;
  if (!partialMd5(stack->fs, epub->getPath().c_str(), id)) {
    LOG_ERR("BORGES", "No identity for this book: nothing published");
    return false;
  }
  auto input = makeUniqueNoThrow<FinalizeInput>();  // Holds a Position: too large for the stack.
  auto session = makeUniqueNoThrow<SessionSummary>();
  if (!input || !session || !stack->openEngine()) return false;

  // This exit answers for all of it: whatever an earlier restart left is in the tracker by now, or
  // was too old to believe, and must not come back at the next open.
  const std::string kept = trackedPath(*epub);
  if (stack->fs.exists(kept.c_str())) stack->fs.remove(kept.c_str());

  XpointerBudget budget(renderer, *epub);

  input->book.id = id;
  input->book.title = epub->getTitle().c_str();
  input->book.author = epub->getAuthor().c_str();
  input->book.file = fileNameOf(epub->getPath());
  input->book.totalPages = snapshot.totalPages;
  input->book.totalPagesApproximate = snapshot.totalPagesApproximate;

  const WallTime now = stack->clock.wallNow();
  input->paged = readingPublishesPosition(tracker, snapshot.percent);
  if (input->paged) {
    input->position.setPercent(snapshot.percent);
    input->position.page = snapshot.page;
    input->position.totalPages = snapshot.totalPages;
    // Not bound by the count: the position comes first, and is one resolve.
    if (snapshot.mapped && budget.take(false, snapshot.mapped->spineIndex)) {
      const KOReaderPosition mapped = ProgressMapper::toKOReader(epub, *snapshot.mapped);
      if (mapped.valid && mapped.xpath.size() < kMaxXpointer) {
        strncpy(input->position.xpointer, mapped.xpath.c_str(), kMaxXpointer - 1);
      }
    }
  }
  input->lastTurn.precision = now.precision;
  input->lastTurn.epochSec = tracker.lastTurnEpoch > 0 ? tracker.lastTurnEpoch : now.epochSec;
  input->hasLastTurnMonotonic = tracker.paged && tracker.lastTurnMs != 0;
  input->lastTurnMonotonicMs = tracker.lastTurnMs;

  if (tracker.paged && tracker.startedEpoch > kPlausibleEpoch) {
    session->started.epochSec = tracker.startedEpoch;
    session->started.precision = now.precision;
    session->ended = input->lastTurn;
    session->pagesRead = tracker.pagesRead;
    session->startPage = tracker.startPage;
    session->endPage = snapshot.page;
    input->session = session.get();
  }

  tracker.leavePage();
  input->pageStats = &tracker.pageStats;

  ClipSource clips(*stack, epub, budget);
  input->clips = &clips;
  const FinalizeResult result = stack->engine.finalizeBook(*input);
  // Only what really failed to reach the queue. A visit too short to be a session is not one of
  // those, however the clippings went: keeping it would bring its start time back at the next open.
  const bool progressLost = result.progressFailed;
  const bool sessionLost = result.sessionFailed;
  if (progressLost || sessionLost || tracker.pageStats.count() > 0) {
    // Clippings retry by themselves at the next finalize. These live only in this reader.
    TrackedReading keep = tracker;
    keep.paged = progressLost;
    if (!sessionLost) {
      keep.pagesRead = 0;
      keep.startedEpoch = 0;
    }
    if (!saveTrackedReading(stack->fs, kept.c_str(), keep, &tracker.pageStats)) {
      LOG_ERR("BORGES", "Reading state lost");
    }
  }
  LOG_DBG("BORGES", "Finalize ok=%d progress=%d created=%u deleted=%u", result.ok ? 1 : 0,
          result.progressQueued ? 1 : 0, result.annotationsCreated, result.annotationsDeleted);
  return budget.frameLent();
}

void keepReadingAcrossRestart(ReaderTracker& tracker, const Epub& epub) {
  if (!mayBePaired()) return;
  tracker.leavePage();
  if (!tracker.paged && tracker.pageStats.count() == 0) return;
  net::BorgesFsHal fs;
  if (!saveTrackedReading(fs, trackedPath(epub).c_str(), tracker, &tracker.pageStats)) {
    LOG_ERR("BORGES", "Reading state lost");
  }
}

void discardKeptReading(const Epub& epub) {
  net::BorgesFsHal fs;
  const std::string path = trackedPath(epub);
  if (fs.exists(path.c_str())) fs.remove(path.c_str());
}

void restoreReading(ReaderTracker& tracker, const Epub& epub) {
  if (!mayBePaired()) return;
  net::BorgesFsHal fs;
  TrackedReading kept;
  if (takeTrackedReading(fs, trackedPath(epub).c_str(), kept, &tracker.pageStats) &&
      keptReadingStillCurrent(kept, static_cast<int64_t>(time(nullptr)))) {
    tracker.paged = kept.paged;
    tracker.pagesRead = kept.pagesRead;
    tracker.startPage = kept.startPage;
    if (kept.startedEpoch > kPlausibleEpoch) tracker.startedEpoch = kept.startedEpoch;
    tracker.lastTurnEpoch = kept.lastTurnEpoch;
  }
}

void seedClipsAtOpen(const Epub& epub) {
  if (!Storage.exists(kAccountMovedMarkerPath)) return;
  auto stack = makeStack();
  if (!stack || !stack->openStore() || !stack->store.creds().paired()) return;
  BookId id;
  if (!partialMd5(stack->fs, epub.getPath().c_str(), id)) return;
  ClipKeys keys;
  // On failure the finalize does it instead, at the cost of what this reading highlights.
  if (!stack->engine.seedClipsIfForeign(id, keys)) LOG_ERR("BORGES", "Could not adopt this book's clippings");
}

bool takeReaderCheckNotice() {
  if (!mayBePaired()) return false;
  net::BorgesFsHal fs;
  GlueStore store(fs);
  GlueState state;
  if (!store.load(state) || !takeCheckNotice(state)) return false;
  store.save(state);
  LOG_INF("BORGES", "Check notice shown");
  return true;
}

void noteLeftSessionOnPurpose() { sLeftOnPurpose = kLeftOnPurposeMagic; }

void publishRestoredPosition(const std::shared_ptr<Epub>& epub, const Position& position) {
  auto stack = makeStack();
  if (!stack || !stack->openStore() || !stack->store.creds().paired() || !stack->openEngine()) return;
  auto input = makeUniqueNoThrow<FinalizeInput>();
  if (!input || !partialMd5(stack->fs, epub->getPath().c_str(), input->book.id)) return;
  input->book.title = epub->getTitle().c_str();
  input->book.author = epub->getAuthor().c_str();
  input->book.file = fileNameOf(epub->getPath());
  input->position = position;
  input->paged = true;
  input->lastTurn = stack->clock.wallNow();
  // An answer for this book: a kept place from before the jump must not outrun it.
  discardKeptReading(*epub);
  if (!stack->engine.finalizeBook(*input).ok) LOG_ERR("BORGES", "Could not queue the restored place");
}

void stampClip(const std::string& cachePath, const uint16_t spine, const uint32_t uptimeStamp,
               const std::optional<uint32_t> visibleOffset) {
  if (!mayBePaired()) return;
  const time_t now = time(nullptr);
  net::BorgesFsHal fs;
  ClipStamp stamp;
  stamp.spine = spine;
  stamp.uptime = uptimeStamp;
  // 0: the event omits the time rather than state a wrong one.
  stamp.epochMs = static_cast<int64_t>(now) >= kPlausibleEpoch ? static_cast<int64_t>(now) * 1000 : 0;
  stamp.visibleOffset = visibleOffset.value_or(kNoVisibleOffset);
  const std::string path = cachePath + kClipTimes;
  if (!ClipTimes(fs).append(path.c_str(), stamp)) LOG_ERR("BORGES", "Could not stamp a clipping");
}

void stampBookmark(const std::string& cachePath, const uint16_t spine, const float progress,
                   const std::optional<uint32_t> visibleOffset) {
  uint32_t progressBits = 0;
  memcpy(&progressBits, &progress, sizeof(progressBits));
  stampClip(cachePath, spine, progressBits, visibleOffset);
}

bool isPaired() { return Storage.exists(kPairedMarkerPath); }

bool hasUndo(const std::string& bookPath) {
  if (bookPath.empty() || !mayBePaired()) return false;
  return Storage.exists((cachePathFor(bookPath) + kUndoFlag).c_str());
}

bool peekUndo(const std::string& bookPath, Position& out) {
  auto stack = makeStack();
  if (!stack || !stack->openStore() || !stack->openEngine()) return false;
  BookId id;
  if (!partialMd5(stack->fs, bookPath.c_str(), id)) return false;
  return stack->engine.peekUndo(id, out);
}

void consumeUndo(const std::string& bookPath) {
  auto stack = makeStack();
  auto position = makeUniqueNoThrow<Position>();  // Too large for the stack.
  BookId id;
  if (stack && position && stack->openStore() && stack->openEngine() && partialMd5(stack->fs, bookPath.c_str(), id)) {
    stack->engine.takeUndo(id, *position);
  }
  Storage.remove((cachePathFor(bookPath) + kUndoFlag).c_str());
}

std::unique_ptr<Activity> makeHandoff(GfxRenderer& renderer, MappedInputManager& input, const NetPayload payload) {
  return makeUniqueNoThrow<BorgesHandoffActivity>(renderer, input, payload);
}

std::unique_ptr<Activity> makeStatusActivity(GfxRenderer& renderer, MappedInputManager& input) {
  return makeUniqueNoThrow<BorgesActivity>(renderer, input, false, NetPayload::Status);
}

void startManualFromShortcut(GfxRenderer& renderer, MappedInputManager& input, const bool inReader) {
  // "Sync now" does that wherever it is pressed. Only with nothing to sync with does it show why.
  auto next = !isPaired()
                  ? makeStatusActivity(renderer, input)
                  : makeHandoff(renderer, input, inReader ? NetPayload::ManualFromReader : NetPayload::ManualFromHome);
  if (next) activityManager.replaceActivity(std::move(next));
}

}  // namespace glue
}  // namespace borges
