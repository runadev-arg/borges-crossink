#include "BorgesGlue.h"

#include <BookmarkStore.h>
#include <BuildScratch.h>
#include <Epub.h>
#include <Epub/Section.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <InflateStream.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdlib>
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
#include "Inbound.h"
#include "ProgressMapper.h"
#include "ReaderTracker.h"
#include "Underlines.h"
#include "activities/ActivityManager.h"
#include "activities/home/RecentBookProgress.h"
#include "activities/reader/EpubReaderUtils.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

#ifdef SIMULATOR
#include <HalDisplay.h>
extern HalDisplay display;
extern "C" int SDL_PollEvent(void* event);
#endif

namespace borges {
namespace glue {

namespace {

#ifdef SIMULATOR
// On a desktop the page is back before a screenshot can see a notice meant to last while a page
// builds. Only the main loop presents and takes commands, and this is the main loop.
void holdForScreenshot() {
  if (!getenv("BORGES_SIM_HOLD_JUMP_NOTICE")) return;
  for (int i = 0; i < 30; ++i) {
    display.presentIfNeeded();
    SDL_PollEvent(nullptr);
    delay(50);
  }
}
#endif

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

// What this reader highlighted itself, by asking its clippings. Only those in the chapter asked
// about are read, once each: most of a long list stays on the card.
class OwnClips final : public IOwnPassages {
 public:
  bool isOwn(const uint64_t passage, const int32_t spine) override {
    const size_t clips = CLIPPINGS.clippingCount();
    if (clips == 0 || passage == 0) return false;
    if (!hashes_) hashes_ = makeUniqueNoThrow<uint64_t[]>(clips);
    if (!hashes_) return false;
    if (!cleared_) {
      memset(hashes_.get(), 0, clips * sizeof(uint64_t));
      cleared_ = true;
    }
    for (size_t i = 0; i < clips; ++i) {
      const Clipping* clip = CLIPPINGS.clippingAt(i);
      if (!clip || (spine >= 0 && clip->spineIndex != spine)) continue;
      if (hashes_[i] == 0) {
        // std::string::resize aborts when it cannot grow, and a passage listed although it is also
        // here is only a row too many.
        if (ESP.getMaxAllocHeap() < clip->textLength + kAllocationMargin || !CLIPPINGS.readClippingText(i, text_)) {
          continue;
        }
        hashes_[i] = passageHash(text_.c_str());
      }
      if (hashes_[i] == passage) return true;
    }
    return false;
  }

 private:
  std::unique_ptr<uint64_t[]> hashes_;
  std::string text_;
  bool cleared_ = false;
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

std::unique_ptr<IOwnPassages> makeOwnPassages() { return makeUniqueNoThrow<OwnClips>(); }

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

namespace {
void publishPosition(BorgesStack& stack, const std::shared_ptr<Epub>& epub, const Position& position) {
  auto input = makeUniqueNoThrow<FinalizeInput>();
  if (!input || !partialMd5(stack.fs, epub->getPath().c_str(), input->book.id)) return;
  input->book.title = epub->getTitle().c_str();
  input->book.author = epub->getAuthor().c_str();
  input->book.file = fileNameOf(epub->getPath());
  input->position = position;
  input->paged = true;
  input->lastTurn = stack.clock.wallNow();
  // An answer for this book: a kept place from before the jump must not outrun it.
  discardKeptReading(*epub);
  if (!stack.engine.finalizeBook(*input).ok) LOG_ERR("BORGES", "Could not queue the restored place");
}
}  // namespace

void publishRestoredPosition(const std::shared_ptr<Epub>& epub, const Position& position) {
  auto stack = makeStack();
  if (!stack || !stack->openStore() || !stack->store.creds().paired() || !stack->openEngine()) return;
  publishPosition(*stack, epub, position);
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

bool hasInboundHighlights(const std::string& bookPath) {
  // No RTC, no file: a boot starts over, and every sync is a boot.
  static uint32_t knownKey = 0;
  static bool knownAnswer = false;
  if (bookPath.empty() || !mayBePaired()) return false;
  const uint32_t key =
      pathHash(bookPath.c_str()) ^ (static_cast<uint32_t>(CLIPPINGS.clippingCount()) * 2654435761u) ^ 1u;
  if (key == knownKey) return knownAnswer;
  knownKey = key;
  knownAnswer = false;
  net::BorgesFsHal fs;
  BookId id;
  if (!partialMd5(fs, bookPath.c_str(), id) || !InboundList::present(fs, id)) return false;
  auto stack = makeStack();
  if (!stack || !stack->openStore()) return false;
  auto own = makeOwnPassages();
  knownAnswer = InboundList::hasAny(stack->fs, id, stack->store.state().accountEpoch, own.get());
  LOG_INF("BORGES", "Highlights of other readers in this book: %d", knownAnswer ? 1 : 0);
  return knownAnswer;
}

namespace {

constexpr size_t kUnderlineWholeSectionBytes = 384 * 1024;

std::string underlineSpinesPath(const Epub& epub) { return epub.getCachePath() + "/ulb.bin"; }

std::string underlinePlanPath(const Epub& epub, const int spine) {
  return epub.getCachePath() + "/ulp" + std::to_string(spine) + ".bin";
}

// What this reader has highlighted in a chapter, from what is already in RAM.
uint32_t ownClipsSignature(const int spine) {
  uint32_t signature = 2166136261u;
  const size_t clips = CLIPPINGS.clippingCount();
  for (size_t i = 0; i < clips; ++i) {
    const Clipping* clip = CLIPPINGS.clippingAt(i);
    if (!clip || clip->spineIndex != spine) continue;
    signature = (signature ^ clip->textOffset) * 16777619u;
    signature = (signature ^ clip->textLength) * 16777619u;
    signature = (signature ^ clip->timestamp) * 16777619u;
  }
  return signature;
}

class UnderlineWalk final : public ChapterXPathResolver::BatchListener {
 public:
  explicit UnderlineWalk(UnderlinePlanner& planner) : planner_(planner) {}
  void onHit(const uint16_t id, const uint32_t at) override { planner_.onHit(id, at); }
  bool onChar(const uint32_t cp, const uint32_t at) override { return planner_.onChar(cp, at); }

 private:
  UnderlinePlanner& planner_;
};

}  // namespace

std::unique_ptr<UnderlineBook> openUnderlines(const Epub& epub) {
  net::BorgesFsHal fs;
  if (!InboundList::anyOnCard(fs)) return nullptr;
  BookId id;
  if (!partialMd5(fs, epub.getPath().c_str(), id) || !InboundList::present(fs, id)) return nullptr;
  // An optimized copy numbers its spine items its own way, and a highlight names the original's.
  if (epub.requiresSourceSpineMap()) return nullptr;
  uint16_t epoch = 0;
  {
    auto stack = makeStack();
    if (!stack || !stack->openStore()) return nullptr;
    epoch = stack->store.state().accountEpoch;
  }
  uint32_t identity = 0;
  if (!InboundList::identity(fs, id, epoch, identity)) return nullptr;
  auto book = makeUniqueNoThrow<UnderlineBook>();
  if (!book) {
    LOG_ERR("BORGES", "No memory to underline this book");
    return nullptr;
  }
  book->book = id;
  book->epoch = epoch;
  book->logIdentity = identity;
  const std::string path = underlineSpinesPath(epub);
  if (!loadUnderlineSpines(fs, path.c_str(), identity, book->spineBits)) {
    if (!collectUnderlineSpines(fs, id, epoch, book->spineBits)) {
      LOG_ERR("BORGES", "Could not read this book's highlights to underline them");
      return nullptr;
    }
    // Only time is lost if this does not stay: it is done again at the next open.
    if (!saveUnderlineSpines(fs, path.c_str(), identity, book->spineBits))
      LOG_ERR("BORGES", "Could not keep %s", path.c_str());
  }
  bool any = false;
  for (const uint8_t bits : book->spineBits) any = any || bits != 0;
  LOG_INF("BORGES", "Underlines for this book: %d", any ? 1 : 0);
  if (!any) return nullptr;
  return book;
}

bool underlinesWantWholeSection(const UnderlineBook& book, const Epub& epub, const int spine) {
  if (!underlineSpineBit(book.spineBits, spine)) return false;
  const size_t upTo = epub.getCumulativeSpineItemSize(spine);
  const size_t before = spine > 0 ? epub.getCumulativeSpineItemSize(spine - 1) : 0;
  return upTo - before <= kUnderlineWholeSectionBytes;
}

void prepareUnderlines(UnderlineBook& book, GfxRenderer& renderer, const std::shared_ptr<Epub>& epub,
                       const Section& section, const int spine, const uint32_t layoutSignature) {
  if (book.planTried && book.planSpine == spine && book.planLayout == layoutSignature) return;
  book.planSpine = spine;
  book.planLayout = layoutSignature;
  book.planTried = true;
  book.planSegments = 0;
  book.windowFirst = 1;
  book.windowEnd = 0;
  book.windowCount = 0;
  memset(book.pageBits, 0, sizeof(book.pageBits));
  if (!underlineSpineBit(book.spineBits, spine)) return;
  if (section.isBuilding() || section.isPartial() || section.pageCount == 0) return;

  UnderlinePlanKey key;
  key.log = book.logIdentity;
  key.own = ownClipsSignature(spine);
  key.layout = layoutSignature;
  key.pages = section.pageCount;
  const std::string path = underlinePlanPath(*epub, spine);
  net::BorgesFsHal fs;
  if (loadUnderlinePlan(fs, path.c_str(), key, book.planSegments, book.pageBits)) return;
  if (section.pageCount > kUnderlineMaxPages) {
    LOG_ERR("BORGES", "Spine %d has %u pages, more than can be underlined", spine, section.pageCount);
    return;
  }

  const uint32_t startedAt = millis();
  XpointerBudget budget(renderer, *epub);
  if (!budget.take(false, spine)) return;

  // Room for all of them only until it is known how many there are: most chapters hold a few, and the
  // walk needs the heap.
  auto targets = makeUniqueNoThrow<UnderlineTarget[]>(kUnderlineMaxTargets);
  if (!targets) {
    LOG_ERR("BORGES", "No memory to find this chapter's highlights");
    return;
  }
  uint16_t targetCount = 0;
  {
    auto own = makeOwnPassages();
    if (!collectUnderlineTargets(fs, book.book, book.epoch, spine, own.get(), &ChapterXPathResolver::pointerKey,
                                 targets.get(), kUnderlineMaxTargets, targetCount)) {
      LOG_ERR("BORGES", "Could not read this chapter's highlights");
      return;
    }
  }
  if (targetCount > 0 && targetCount < kUnderlineMaxTargets) {
    auto exact = makeUniqueNoThrow<UnderlineTarget[]>(targetCount);
    if (exact) {
      memcpy(static_cast<void*>(exact.get()), targets.get(), targetCount * sizeof(UnderlineTarget));
      targets = std::move(exact);
    }
  }
  uint16_t segmentCount = 0;
  bool walked = true;
  uint16_t verified = 0;
  uint16_t refused = 0;
  std::unique_ptr<UnderlineSegment[]> segments;
  if (targetCount > 0) {
    const uint16_t segmentCap = targetCount >= kUnderlineMaxSegments / 3 ? kUnderlineMaxSegments : targetCount * 3;
    const auto pages = makeUniqueNoThrow<uint32_t[]>(section.pageCount);
    const auto pointers = makeUniqueNoThrow<ChapterXPathResolver::BatchPointer[]>(targetCount);
    segments = makeUniqueNoThrow<UnderlineSegment[]>(segmentCap);
    // What the walk itself needs has to be left after these.
    if (!pages || !pointers || !segments || ESP.getFreeHeap() < kMinFreeForXpointer ||
        ESP.getMaxAllocHeap() < kMinBlockForXpointer) {
      LOG_ERR("BORGES", "No memory to underline this chapter (free=%u maxAlloc=%u)", ESP.getFreeHeap(),
              ESP.getMaxAllocHeap());
      return;
    }
    if (!section.readVisibleOffsets(pages.get(), section.pageCount)) {
      LOG_ERR("BORGES", "Could not read where this chapter's pages start");
      return;
    }
    for (uint16_t i = 0; i < targetCount; ++i) {
      pointers[i].key = targets[i].key;
      pointers[i].offset = targets[i].offset;
      pointers[i].id = i;
    }
    UnderlinePlanner planner(targets.get(), targetCount, pages.get(), section.pageCount, segments.get(), segmentCap);
    UnderlineWalk walk(planner);
    walked = ChapterXPathResolver::walkPointers(epub, spine, pointers.get(), targetCount, walk);
    segmentCount = planner.finish();
    verified = planner.verified();
    refused = planner.refused();
  }
  const bool timedOut = ChapterXPathResolver::deadlineHit();
  // A chapter too long to walk in time would be walked again at every entry: it stays without, until
  // the log or the layout changes. One that could not be read says nothing and is tried again.
  if (timedOut) segmentCount = 0;
  if (!walked && !timedOut) return;
  if (!saveUnderlinePlan(fs, path.c_str(), key, segments.get(), segmentCount)) {
    LOG_ERR("BORGES", "Could not keep %s", path.c_str());
  }
  book.planSegments = segmentCount;
  for (uint16_t i = 0; i < segmentCount; ++i) {
    const uint32_t bit = segments[i].page % (kUnderlinePageBytes * 8);
    book.pageBits[bit / 8] = static_cast<uint8_t>(book.pageBits[bit / 8] | (1u << (bit % 8)));
  }
  LOG_INF("BORGES",
          "Underline plan spine=%d targets=%u verified=%u refused=%u segments=%u timedOut=%d in %lu ms free=%u", spine,
          targetCount, verified, refused, segmentCount, timedOut ? 1 : 0,
          static_cast<unsigned long>(millis() - startedAt), ESP.getFreeHeap());
}

bool underlinesForPage(UnderlineBook& book, const Epub& epub, const int spine, const uint32_t layoutSignature,
                       const int page, const UnderlineSegment*& segments, uint8_t& count) {
  if (book.planSpine != spine || book.planLayout != layoutSignature || book.planSegments == 0 || page < 0) return false;
  const auto at = static_cast<uint32_t>(page);
  const uint32_t bit = at % (kUnderlinePageBytes * 8);
  if ((book.pageBits[bit / 8] & (1u << (bit % 8))) == 0) return false;
  if (at < book.windowFirst || at >= book.windowEnd) {
    book.windowFirst = at;
    book.windowEnd = at + 1;
    book.windowCount = 0;
    // On the stack: this is on the way to a page, where what the heap has left is not to be spent.
    char path[160];
    const int length = snprintf(path, sizeof(path), "%s/ulp%d.bin", epub.getCachePath().c_str(), spine);
    if (length <= 0 || length >= static_cast<int>(sizeof(path))) return false;
    net::BorgesFsHal fs;
    book.windowCount = readUnderlineWindow(fs, path, book.planSegments, static_cast<uint16_t>(page), book.window,
                                           kUnderlinePageMax, book.windowEnd);
  }
  count = 0;
  segments = nullptr;
  for (uint8_t i = 0; i < book.windowCount; ++i) {
    if (book.window[i].page != at) continue;
    if (!segments) segments = &book.window[i];
    ++count;
  }
  return count > 0;
}

std::unique_ptr<Activity> makeInboundActivity(GfxRenderer& renderer, MappedInputManager& input,
                                              const std::string& bookPath) {
  return makeUniqueNoThrow<BorgesInboundActivity>(renderer, input, bookPath);
}

HighlightJumpOutcome jumpToHighlight(GfxRenderer& renderer, ReaderTracker& tracker, const std::shared_ptr<Epub>& epub,
                                     const CrossPointPosition& here, const float herePercent,
                                     const std::string& xpointer, const bool hasPct, const uint32_t pctMicro) {
  const int spines = epub->getSpineItemsCount();
  const int32_t namedSpine = spineOfXpointer(xpointer.c_str());
  const bool hasPlace = namedSpine >= 0 && namedSpine < spines;
  if (!hasPlace && !hasPct) {
    LOG_ERR("BORGES", "This highlight says nowhere to go");
    return HighlightJumpOutcome::Failed;
  }
  auto stack = makeStack();
  auto before = makeUniqueNoThrow<Position>();  // Too large for the stack.
  auto after = makeUniqueNoThrow<Position>();
  BookId id;
  if (!stack || !before || !after || !stack->openStore() || !stack->openEngine() ||
      !partialMd5(stack->fs, epub->getPath().c_str(), id)) {
    return HighlightJumpOutcome::Failed;
  }

  // Mapping streams a chapter and takes moments on a big one, with nothing else on the glass.
  GUI.drawPopup(renderer, tr(STR_BORGES_ONE_MOMENT));
  CrossPointPosition mapped{};
  {
    XpointerBudget budget(renderer, *epub);
    if (budget.take(false, here.spineIndex)) {
      const KOReaderPosition local = ProgressMapper::toKOReader(epub, here);
      if (local.valid && local.xpath.size() < kMaxXpointer)
        strncpy(before->xpointer, local.xpath.c_str(), kMaxXpointer - 1);
    }
    if (herePercent >= 0) before->setPercent(herePercent);
    KOReaderPosition target;
    target.xpath = hasPlace ? xpointer : "";
    target.percentage = hasPct ? static_cast<float>(pctMicro) / 100000000.0f : 0.0f;
    // Without the memory for it the mapper finds no match, and falls back on the percentage.
    if (hasPlace) budget.take(false, namedSpine);
    mapped = ProgressMapper::toCrossPoint(epub, target, here.spineIndex, here.totalPages);
  }

  // From here on the frame is whatever the loan left in it.
  const auto failed = [&renderer](const char* why) {
    LOG_ERR("BORGES", "%s", why);
    renderer.clearScreen(ReaderUtils::readerBackgroundColor());
    return HighlightJumpOutcome::Failed;
  };
  SavedPosition saved;
  if (!mapped.valid ||
      !normalizeForSave(mapped.spineIndex, mapped.pageNumber, mapped.totalPages, epub->getSpineItemsCount(), saved)) {
    return failed("This highlight has no place in this book");
  }
  // Read nothing since the last jump, and the way back is still the place the reading was at.
  const bool keepExisting = !tracker.paged && hasUndo(epub->getPath());
  if (!stack->engine.setUndo(id, *before, keepExisting)) return failed("Could not keep the way back");
  const std::optional<uint32_t> offset =
      mapped.hasVisibleTextOffset ? std::optional<uint32_t>(mapped.visibleTextOffset) : std::nullopt;
  if (!EpubReaderUtils::saveProgress(*epub, saved.spine, saved.page, saved.pageCount, offset)) {
    return failed("Could not save the place of the highlight");
  }
  RecentBookProgress::saveCachedEpubPercent(*epub, saved.spine, saved.page, saved.pageCount);
  HalFile flag;
  if (Storage.openFileForWrite("BORGES", epub->getCachePath() + kUndoFlag, flag)) flag.close();

  // It counts as any way of getting to a page does, as in the plugin: it is where this reader is.
  if (hasPct) {
    after->hasPct = true;
    after->pctMicro = pctMicro;
  } else {
    after->setPercent(epub->calculateProgress(saved.spine, 0) * 100.0);
  }
  if (hasPlace) strncpy(after->xpointer, xpointer.c_str(), kMaxXpointer - 1);
  discardKeptReading(*epub);
  if (stack->store.creds().paired()) publishPosition(*stack, epub, *after);

  const bool exact = mapped.hasVisibleTextOffset;
  // The page is built from nothing after this. Meanwhile, what just happened and how to undo it.
  renderer.clearScreen(ReaderUtils::readerBackgroundColor());
  const Rect area = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  Rect text = area;
  text.x += kSideMargin;
  text.width -= kSideMargin * 2;
  int y = area.y + area.height / 3;
  if (!exact) y = drawWrappedCentered(renderer, text, UI_10_FONT_ID, y, tr(STR_BORGES_WARN_APPROXIMATE), 3) + kBlockGap;
  drawWrappedCentered(renderer, text, SMALL_FONT_ID, y, tr(STR_BORGES_JUMP_TIP), 4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  LOG_INF("BORGES", "Went to a highlight: spine=%d exact=%d", saved.spine, exact ? 1 : 0);
#ifdef SIMULATOR
  holdForScreenshot();
#endif
  return exact ? HighlightJumpOutcome::Exact : HighlightJumpOutcome::Approximate;
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
