#include "BorgesGlue.h"

#include <BookmarkStore.h>
#include <BuildScratch.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <InflateStream.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>
#include <ctime>
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
// With the framebuffer lent, inflate's 43 KB come out of it. What is left on the heap is the
// XML parser, its string pools and the read chunk.
constexpr uint32_t kMinFreeForXpointer = 16 * 1024;
constexpr uint32_t kMinBlockForXpointer = 8 * 1024;

std::string cachePathFor(const std::string& path) { return Epub::cachePathForFilePath(path, kCacheRoot); }
constexpr uint32_t kAllocationMargin = 2 * 1024;

const char* fileNameOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path.c_str() : path.c_str() + slash + 1;
}

// Holds the book before anything can go wrong on the way to its check: no link, a press
// that skips the session, a crash in that boot. The hold is what keeps stale progress from
// quietly becoming the head at the next sleep.
void holdBook(BorgesStack& stack, const std::string& path) {
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
    const Gate gate = stack.engine.gate(SessionKind::Open);
    in.needsRelink = gate == Gate::NeedsRelink;
    in.backedOff = gate == Gate::BackedOff;
  }
  in.hasSavedWifi = hasSavedWifi();
  in.autoOnOpen = stack.glue.autoOnOpen;
  in.throttled = openThrottled(stack.glue, pathHash(path.c_str()), stack.clock.wallNow());
  OpenRoute route = routeOpen(in);
  if (route != OpenRoute::Direct) holdBook(stack, path);
  if (route == OpenRoute::NetworkSession) {
    if (!armSession(stack.glue, PendingKind::OpenBook)) {
      LOG_INF("BORGES", "Open sessions keep failing: opening without one");
      route = OpenRoute::DirectWithHold;
    }
    stack.glueStore.save(stack.glue);
  }
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
  explicit XpointerBudget(GfxRenderer& renderer) : renderer_(renderer), startedAt_(millis()) {}

  // False: not now. Whoever asked retries at a later finalize, or goes without.
  bool take(const bool bounded) {
    if (bounded && (taken_ >= kMaxXpointersPerFinalize || millis() - startedAt_ >= kXpointerBudgetMs)) return false;
    if (!loan_) loan_ = makeUniqueNoThrow<GfxRenderer::FrameBufferLoan>(renderer_);
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
  GfxRenderer& renderer_;
  std::unique_ptr<GfxRenderer::FrameBufferLoan> loan_;
  uint32_t startedAt_;
  int taken_ = 0;
};

class ClipSource final : public IClipSource {
 public:
  ClipSource(BorgesStack& stack, std::shared_ptr<Epub> epub, XpointerBudget& budget)
      : stack_(stack), epub_(std::move(epub)), budget_(budget) {
    timesPath_ = epub_->getCachePath() + kClipTimes;
  }

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
      out.textHash = clipIdentity(clip->timestamp, clip->textLength, clip->wordCount);
      out.bookmark = false;
      return true;
    }
    const auto& bookmarks = BOOKMARKS.getBookmarks();
    if (index - clips >= bookmarks.size()) return false;
    const Bookmark& mark = bookmarks[index - clips];
    out = bookmarkKey(mark.spineIndex, mark.paragraphIndex, mark.progress);
    return true;
  }

  bool detail(const size_t index, ClipDetail& out) override {
    // The core treats false as "try again at the next finalize", and nothing is lost.
    const size_t clips = CLIPPINGS.clippingCount();
    uint16_t spine = 0;
    uint16_t paragraph = UINT16_MAX;
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
      out.page = clip->startPage;
      out.timeMs = ClipTimes(stack_.fs).lookup(timesPath_.c_str(), spine, clip->timestamp);
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
    }
    out.chapter = chapter_.c_str();
    xpointer_.clear();
    if (paragraph != UINT16_MAX) {
      // Published without one it would stay without one: the ledger never asks again.
      if (!budget_.take(true)) return false;
      xpointer_ = ChapterXPathResolver::findXPathForParagraph(epub_, spine, paragraph);
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
  in.gateGo = stack->engine.gate(SessionKind::Sleep) == Gate::Go;
  in.batteryPercent = batteryPercent;
  if (!glue::shouldFlushBeforeSleep(in)) return false;
  const bool armed = armSession(stack->glue, PendingKind::SleepFlush);
    case NetPayload::Disconnect:
    case NetPayload::OtherDevice:
  stack->glueStore.save(stack->glue);
  return armed;
}

bool finalizeFromReader(GfxRenderer& renderer, const ReaderTracker& tracker, const std::shared_ptr<Epub>& epub,
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

  XpointerBudget budget(renderer);

  input->book.id = id;
  input->book.title = epub->getTitle().c_str();
  input->book.author = epub->getAuthor().c_str();
  input->book.file = fileNameOf(epub->getPath());
  input->book.totalPages = snapshot.totalPages;

  const WallTime now = stack->clock.wallNow();
  input->paged = tracker.paged && snapshot.percent >= 0;
  if (input->paged) {
    input->position.setPercent(snapshot.percent);
    input->position.page = snapshot.page;
    input->position.totalPages = snapshot.totalPages;
    // Unbounded by count and time: the position comes first, and is one resolve.
    if (snapshot.mapped && budget.take(false)) {
      const KOReaderPosition mapped = ProgressMapper::toKOReader(epub, *snapshot.mapped);
      if (mapped.valid && mapped.xpath.size() < kMaxXpointer) {
        strncpy(input->position.xpointer, mapped.xpath.c_str(), kMaxXpointer - 1);
      }
    }
  }
  input->lastTurn.precision = now.precision;
  input->lastTurn.epochSec = tracker.lastTurnEpoch > 0 ? tracker.lastTurnEpoch : now.epochSec;
  input->hasLastTurnMonotonic = tracker.paged;
  input->lastTurnMonotonicMs = tracker.lastTurnMs;

  if (tracker.paged && tracker.startedEpoch > kPlausibleEpoch) {
    session->started.epochSec = tracker.startedEpoch;
    session->started.precision = now.precision;
    session->ended = input->lastTurn;
    session->pagesRead = tracker.pagesRead;
    session->endPage = snapshot.page;
    input->session = session.get();
  }

  ClipSource clips(*stack, epub, budget);
  input->clips = &clips;
  const FinalizeResult result = stack->engine.finalizeBook(*input);
  LOG_DBG("BORGES", "Finalize ok=%d progress=%d created=%u deleted=%u", result.ok ? 1 : 0,
          result.progressQueued ? 1 : 0, result.annotationsCreated, result.annotationsDeleted);
  return budget.frameLent();
}

void stampClip(const std::string& cachePath, const uint16_t spine, const uint32_t uptimeStamp) {
  const time_t now = time(nullptr);
  if (static_cast<int64_t>(now) < kPlausibleEpoch || !mayBePaired()) return;  // Otherwise the event omits the time.
  net::BorgesFsHal fs;
  ClipStamp stamp;
  stamp.spine = spine;
  stamp.uptime = uptimeStamp;
  stamp.epochMs = static_cast<int64_t>(now) * 1000;
  const std::string path = cachePath + kClipTimes;
  if (!ClipTimes(fs).append(path.c_str(), stamp)) LOG_ERR("BORGES", "Could not stamp a clipping");
}

bool isPaired() { return mayBePaired(); }

bool hasUndo(const std::string& bookPath) {
  if (bookPath.empty() || !mayBePaired()) return false;
  return Storage.exists((cachePathFor(bookPath) + kUndoFlag).c_str());
}

bool takeUndo(const std::string& bookPath, Position& out) {
  auto stack = makeStack();
  if (!stack || !stack->openStore() || !stack->openEngine()) return false;
  BookId id;
  if (!partialMd5(stack->fs, bookPath.c_str(), id)) return false;
  const bool taken = stack->engine.takeUndo(id, out);
  Storage.remove((cachePathFor(bookPath) + kUndoFlag).c_str());
  return taken;
}

std::unique_ptr<Activity> makeHandoff(GfxRenderer& renderer, MappedInputManager& input, const NetPayload payload) {
  return makeUniqueNoThrow<BorgesHandoffActivity>(renderer, input, payload);
}

std::unique_ptr<Activity> makeStatusActivity(GfxRenderer& renderer, MappedInputManager& input) {
  return makeUniqueNoThrow<BorgesActivity>(renderer, input, false, NetPayload::Status);
}

void startManualFromShortcut(GfxRenderer& renderer, MappedInputManager& input, const bool inReader) {
  auto next = inReader && mayBePaired() ? makeHandoff(renderer, input, NetPayload::ManualFromReader)
                                        : makeStatusActivity(renderer, input);
  if (next) activityManager.replaceActivity(std::move(next));
}

}  // namespace glue
}  // namespace borges
