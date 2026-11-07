#include <Epub.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "BorgesActivities.h"
#include "BorgesUi.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub/Section.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "SilentRestart.h"
#include "activities/home/RecentBookProgress.h"
#include "activities/reader/EpubReaderUtils.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "net/NetRecords.h"

namespace borges {
namespace glue {

namespace {

// The contract is 12 s from the press to the book, and a network boot and the reboot back cost
// about 1.2 s each. So this is what is left, with half a second for an uninterruptible step.
constexpr uint32_t kOpenBudgetMs = 9000;
constexpr uint32_t kOpenLastStepMs = 500;
constexpr uint32_t kStayBudgetMs = 9000;
constexpr uint32_t kChoiceLinkBudgetMs = 6000;
// Most answers come within this, on a connection already open. After it, the choice raises
// the link again by itself: a prompt can sit for an hour on a nightstand.
constexpr unsigned long kPromptLinkMs = 20000;
constexpr uint32_t kSleepExitWaitMs = 4000;
constexpr uint32_t kLeaveWaitMs = 400;
// Past a budget a session sees through ICancel, the one step it cannot be interrupted in.
constexpr unsigned long kHangMarginMs = 12000;
constexpr uint32_t kSleepBudgetMs = 15000;
constexpr uint32_t kSleepLinkBudgetMs = 6000;
// One uninterruptible TCP connect, about 3.5 s, on top of the budget.
constexpr uint32_t kSleepHardDeadlineMs = kSleepBudgetMs + 4000;
constexpr uint32_t kNoticeMs = 3000;
constexpr uint32_t kWakeWaitMs = 600;
constexpr int kActionGap = 8;

enum class CheckOutcome : uint8_t { NotAsked, Answered, Missed };

// A session came back: the guard's count starts over. Takes its own file handles, since
// the session task may still own the stack's.
void completeSession(const bool openSucceeded, const std::string& path, IClock* clock,
                     const CheckOutcome check = CheckOutcome::NotAsked) {
  net::BorgesFsHal fs;
  GlueStore store(fs);
  GlueState state;
  store.load(state);
  finishSession(state);
  if (openSucceeded && clock) noteOpenSuccess(state, pathHash(path.c_str()), clock->wallNow());
  if (check == CheckOutcome::Answered) noteCheckAnswered(state);
  if (check == CheckOutcome::Missed) noteCheckMissed(state, pathHash(path.c_str()));
  store.save(state);
}

bool hasSavedPlace(const std::string& path) {
  const std::string progress = Epub::resolveCachePathForFilePath(path, "/.crosspoint") + "/progress.bin";
  return Storage.exists(progress.c_str()) || Storage.exists((progress + ".bak").c_str());
}

float buildMetadataAndPercent(const std::string& path) {
  auto epub = std::make_shared<Epub>(path, "/.crosspoint");
  epub->setupCacheDir();
  if (!epub->load(true, true, Epub::XLocationLoadMode::Skip, true)) return -1.0f;
  epub.reset();
  return localPercentFor(path);
}

Rect screenArea(const GfxRenderer& renderer) {
  Rect area = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  area.x += kSideMargin;
  area.width -= kSideMargin * 2;
  return area;
}

}  // namespace

// ---------------------------------------------------------------------------------
// Handoff
// ---------------------------------------------------------------------------------

void BorgesHandoffActivity::onEnter() {
  Activity::onEnter();
  if (!bookPath_.empty()) {
    APP_STATE.openEpubPath = bookPath_;
    APP_STATE.saveToFile();
  }
  silentRestartToNetwork(NetworkBootTarget::BORGES, static_cast<uint32_t>(payload_));
}

// ---------------------------------------------------------------------------------
// Open
// ---------------------------------------------------------------------------------

struct BorgesOpenActivity::Session {
  BorgesStack* stack = nullptr;
  std::string path;
  std::string title;
  BookRef ref;
  Position local;
  ResumeDecision decision;
  SessionReport report;
  bool ran = false;
  std::atomic<bool> awaitingChoice{false};
};

BorgesOpenActivity::BorgesOpenActivity(GfxRenderer& renderer, MappedInputManager& input, std::string path)
    : Activity("BorgesOpen", renderer, input), path_(std::move(path)) {}

BorgesOpenActivity::~BorgesOpenActivity() = default;

void BorgesOpenActivity::onEnter() {
  Activity::onEnter();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  title_ = bookTitleFor(path_);
  stack_ = makeStack();
  session_ = makeUniqueNoThrow<Session>();
  if (!stack_ || !session_) {
    openBook(false);
    return;
  }
  session_->stack = stack_.get();
  session_->path = path_;
  session_->title = title_;
  stack_->cancel.arm(kOpenBudgetMs);
  requestUpdate(true);
  if (!worker_.start(&BorgesOpenActivity::sessionMain, session_.get())) openBook(false);
}

void BorgesOpenActivity::sessionMain(void* context) {
  Session& session = *static_cast<Session*>(context);
  BorgesStack& stack = *session.stack;
  if (!stack.openStore() || !stack.openEngine()) return;
  // Nothing cached yet is what the first open after flashing cannot avoid. It gets time for it,
  // counted from the boot, which is already behind it.
  net::LinkHint hint;
  const OpenBudget budget =
      openBudgetFor(stack.store.state().anchorEpoch > 0, net::loadLinkHint(stack.fs, hint) && hint.usable());
  if (budget.totalMs > kOpenBudgetMs) stack.cancel.moveDeadlineSinceBoot(budget.totalMs);
  if (!partialMd5(stack.fs, session.path.c_str(), session.ref.id)) {
    LOG_ERR("BORGES", "No identity for %s: opening without a check", session.path.c_str());
    return;
  }
  // Also here: this boot may have been reached by a path that did not go through routeBook.
  if (!stack.engine.onBookOpened(session.ref.id)) LOG_ERR("BORGES", "Could not hold the book");
  session.ref.title = session.title.c_str();
  const float percent = localPercentFor(session.path);
  if (percent >= 0) session.local.setPercent(percent);
  // A place imported from the old firmware comes without the book's metadata, which is what
  // turns it into a percent. Built here, once per book, and the reader would build it anyway:
  // it must not look like "never opened" to the check.
  if (percent < 0 && hasSavedPlace(session.path)) {
    suspendGuard(stack.glue);
    stack.glueStore.save(stack.glue);
    const float built = buildMetadataAndPercent(session.path);
    if (built >= 0) session.local.setPercent(built);
    resumeGuard(stack.glue);
    stack.glueStore.save(stack.glue);
    stack.cancel.moveDeadline(budget.totalMs);
  }

  const net::LinkResult link = stack.bringUp(budget.linkMs);
  if (link != net::LinkResult::Connected) {
    if (link != net::LinkResult::Cancelled) stack.engine.noteLinkFailure();
    return;
  }
  session.report = stack.engine.runOpenSession(session.ref, session.local, session.decision);
  session.ran = true;
  if (session.report.outcome == SessionOutcome::AwaitingChoice) {
    session.awaitingChoice.store(true, std::memory_order_release);
  } else {
    stack.bringDown();
  }
}

void BorgesOpenActivity::loop() {
  if (mappedInput.wasAnyPressed()) {
    openBook(true);
    return;
  }
  if (worker_.finished()) {
    if (session_->awaitingChoice.load(std::memory_order_acquire)) {
      auto decision = makeUniqueNoThrow<ResumeDecision>(session_->decision);
      if (decision) {
        session_.reset();
        auto resume =
            makeUniqueNoThrow<BorgesResumeActivity>(renderer, mappedInput, std::move(stack_), std::move(decision),
                                                    path_, title_, BorgesResumeActivity::Return::Reader);
        if (resume) {
          activityManager.replaceActivity(std::move(resume));
          return;
        }
      }
    }
    openBook(false);
    return;
  }
  if (stack_ && stack_->cancel.remainingMs() == 0) {
    // The deadline reached the session through ICancel. Give it one uninterruptible step
    // to come back, then stop waiting: a reader never waits on a socket.
    if (!worker_.waitForExit(kOpenLastStepMs)) openBook(false);
  }
}

void BorgesOpenActivity::openBook(const bool pressed) {
  if (stack_) stack_->cancel.request();
  // Only with the session back: a session hung inside the card would hang this too, and a reader
  // never waits on it. Without this write the guard counts it as dead, which it is, unless it was
  // a person who stopped waiting: then it was merely busy.
  if (worker_.waitForExit(kLeaveWaitMs)) {
    const bool ok = session_ && session_->ran && session_->report.outcome == SessionOutcome::Ok;
    // Someone who pressed a button knows they skipped it. Nothing else explains an answer not coming.
    const CheckOutcome check = session_ && session_->report.focusChecked ? CheckOutcome::Answered
                               : pressed                                 ? CheckOutcome::NotAsked
                                                                         : CheckOutcome::Missed;
    completeSession(ok, path_, ok && stack_ ? &stack_->clock : nullptr, check);
    if (stack_) stack_->bringDown();
  } else if (pressed) {
    noteLeftSessionOnPurpose();
  }
  // A reboot turns the radio off and frees everything, wherever the session stands.
  silentRestartToReader(true);
}

void BorgesOpenActivity::onExit() {
  APP_STATE.readerActivityLoadCount = 0;  // Only sleep exits here, and it is not a crash.
  if (stack_) stack_->cancel.request();
  if (!worker_.waitForExit(kSleepExitWaitMs)) {
    // Only reachable on the way to deep sleep, which ends every task within moments.
    (void)stack_.release();
    (void)session_.release();
  } else {
    completeSession(false, path_, nullptr);  // Came back, if only to sleep.
  }
  Activity::onExit();
}

void BorgesOpenActivity::render(RenderLock&&) {
  // The boot asks for a paint too, and what this says never changes.
  if (painted_) return;
  painted_ = true;
  renderer.clearScreen();
  const Rect area = screenArea(renderer);
  const int titleLine = renderer.getLineHeight(UI_12_FONT_ID);
  const int bodyLine = renderer.getLineHeight(UI_10_FONT_ID);
  const int block = titleLine * 2 + kBlockGap + bodyLine;
  int y = area.y + std::max(0, (area.height - block) / 2 - titleLine);
  y = drawWrappedCentered(renderer, area, UI_12_FONT_ID, y, title_.c_str(), 3, EpdFontFamily::BOLD);
  y += kBlockGap;
  drawWrappedCentered(renderer, area, UI_10_FONT_ID, y, tr(STR_BORGES_LOOKING), 2);
  const int hintY = area.y + area.height - renderer.getLineHeight(SMALL_FONT_ID) * 2 - kBlockGap;
  drawWrappedCentered(renderer, area, SMALL_FONT_ID, hintY, tr(STR_BORGES_ANY_BUTTON), 2);
  // Over whatever came before it (a sleep image, the last page): one clean pass, once.
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

// ---------------------------------------------------------------------------------
// Resume
// ---------------------------------------------------------------------------------

BorgesResumeActivity::BorgesResumeActivity(GfxRenderer& renderer, MappedInputManager& input,
                                           std::unique_ptr<BorgesStack> stack, std::unique_ptr<ResumeDecision> decision,
                                           std::string path, std::string title, const Return back)
    : Activity("BorgesResume", renderer, input),
      stack_(std::move(stack)),
      decision_(std::move(decision)),
      path_(std::move(path)),
      title_(std::move(title)),
      back_(back) {}

BorgesResumeActivity::~BorgesResumeActivity() = default;

void BorgesResumeActivity::onEnter() {
  Activity::onEnter();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  linkDownAt_ = millis() + kPromptLinkMs;
  // Going there would move backwards: the one that keeps what was read is under the thumb, not the other.
  focus_ = (decision_->warnings & resume::kWarnBackwards) ? 1 : 0;
  requestUpdate();
}

void BorgesResumeActivity::onExit() {
  APP_STATE.readerActivityLoadCount = 0;  // Only sleep exits here, and it is not a crash.
  if (stack_) stack_->cancel.request();
  if (worker_.waitForExit(kSleepExitWaitMs)) {
    completeSession(false, path_, nullptr);
  } else {
    leaveWorkBehind();
  }
  Activity::onExit();
}

void BorgesResumeActivity::leaveWorkBehind() {
  // Only on the way to deep sleep, which ends every task.
  (void)stack_.release();
  (void)decision_.release();
  (void)work_.release();
}

void BorgesResumeActivity::stayMain(void* context) {
  Work& work = *static_cast<Work*>(context);
  BorgesStack& stack = *work.stack;
  const Position& here = work.stayAt;
  // The radio may have gone down while the prompt waited, or to map the page. Once more,
  // briefly: this device becoming the head is what the reader just asked for. Without a
  // link the choice is still stored, and the next session sends it.
  const bool online = stack.bringUp(kChoiceLinkBudgetMs) == net::LinkResult::Connected;
  stack.engine.applyChoice(*work.decision, ResumeChoice::StayHere, here, here, online);
  stack.bringDown();
}

void BorgesResumeActivity::drainMain(void* context) {
  Work& work = *static_cast<Work*>(context);
  BorgesStack& stack = *work.stack;
  if (stack.bringUp(kChoiceLinkBudgetMs) == net::LinkResult::Connected)
    stack.engine.runAfterChoice(work.decision->book);
  stack.bringDown();
}

void BorgesResumeActivity::loop() {
  switch (state_) {
    case State::CouldNotLocate:
      if (mappedInput.wasAnyPressed() || millis() >= noticeUntil_) {
        stack_->engine.abandonChoice();
        leave(false);
      }
      return;
    case State::Working: {
      const bool pressed = mappedInput.wasAnyPressed();
      if (pressed || worker_.finished() || (workDeadlineAt_ != 0 && millis() >= workDeadlineAt_)) {
        leave(choiceStored_ || worker_.finished(), pressed);
      }
      return;
    }
    case State::Choosing:
      break;
  }

  if (linkDownAt_ != 0 && millis() >= linkDownAt_) {
    stack_->bringDown();
    linkDownAt_ = 0;
  }

  // Back answers nothing: the book opens where it was, and this asks again next time.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    stack_->engine.abandonChoice();
    leave(false, true);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    state_ = State::Working;
    (void)requestUpdateAndWait();
    if (focus_ == 0) {
      goThere();
    } else {
      stayHere();
    }
    return;
  }
  const auto move = [this] {
    focus_ = focus_ == 0 ? 1 : 0;
    requestUpdate();
  };
  navigator_.onNextRelease(move);
  navigator_.onPreviousRelease(move);
}

bool BorgesResumeActivity::applyJump() {
  JumpRequest request;
  if (!planJump(decision_->xpointerUsable, decision_->remoteXpointer, decision_->hasRemotePct, decision_->remotePct,
                request)) {
    return false;
  }
  auto epub = std::make_shared<Epub>(path_, "/.crosspoint");
  epub->setupCacheDir();
  // A book fresh from the library has no cache yet, so this one may build it.
  if (!epub->load(true, true, Epub::XLocationLoadMode::Immediate, true)) {
    LOG_ERR("BORGES", "Could not load the book to place the jump");
    return false;
  }

  EpubReaderUtils::Progress progress;
  const bool hasLocal = EpubReaderUtils::loadProgress(*epub, progress, "BORGES");
  const int pageCount = progress.hasPageCount ? std::max(1, progress.pageCount) : 1;

  // An xpointer is found by streaming its chapter through inflate, whose 32 KB window a C3 just
  // off Wi-Fi has no block for. Without the frame's storage the xpointer goes unresolved and
  // the jump falls to the percentage: KOReader's counts laid out pages, sits before the chapter
  // in bytes, and lands on its first page, as the first real X4 did (page 229 for 239). The panel
  // keeps the prompt; render() repaints from nothing after this.
  GfxRenderer::FrameBufferLoan loan(renderer);

  before_ = makeUniqueNoThrow<Position>(decision_->local);
  if (!before_) return false;
  if (hasLocal) {
    CrossPointPosition local = {progress.spineIndex, progress.pageNumber, pageCount};
    if (progress.hasVisibleTextOffset) {
      local.visibleTextOffset = progress.visibleTextOffset;
      local.hasVisibleTextOffset = true;
    }
    const KOReaderPosition mapped = ProgressMapper::toKOReader(epub, local);
    if (mapped.valid) {
      before_->setPercent(mapped.percentage * 100.0);
      if (mapped.xpath.size() < kMaxXpointer) strncpy(before_->xpointer, mapped.xpath.c_str(), kMaxXpointer - 1);
    }
  }

  KOReaderPosition target;
  target.xpath = request.xpointer;
  target.percentage = request.fraction;
  CrossPointPosition mapped =
      ProgressMapper::toCrossPoint(epub, target, hasLocal ? progress.spineIndex : -1, pageCount);
  if (mapped.valid && mapped.hasVisibleTextOffset) {
    // With a layout on disk the page is known now. Without one the reader finds it from the saved
    // offset once it has laid the chapter out, so a book just downloaded opens at the same place.
    Section section(epub, mapped.spineIndex, renderer);
    if (const auto pages = section.getCachedPageCount(); pages && *pages > 0) {
      mapped.totalPages = *pages;
      if (const auto page = section.getPageForVisibleTextOffset(mapped.visibleTextOffset, true)) {
        mapped.pageNumber = *page;
      }
    }
  }
  loan.end();
  if (!mapped.hasVisibleTextOffset && request.xpointer[0] != '\0') {
    LOG_ERR("BORGES", "The xpointer was not found here, going by the percentage (free=%u maxAlloc=%u)",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }
  SavedPosition saved;
  if (!mapped.valid ||
      !normalizeForSave(mapped.spineIndex, mapped.pageNumber, mapped.totalPages, epub->getSpineItemsCount(), saved)) {
    LOG_ERR("BORGES", "The remote position has no place in this book");
    return false;
  }
  const std::optional<uint32_t> offset =
      mapped.hasVisibleTextOffset ? std::optional<uint32_t>(mapped.visibleTextOffset) : std::nullopt;
  if (!EpubReaderUtils::saveProgress(*epub, saved.spine, saved.page, saved.pageCount, offset)) return false;
  RecentBookProgress::saveCachedEpubPercent(*epub, saved.spine, saved.page, saved.pageCount);
  // A reading kept from before this is over: its place must not replace the jump.
  discardKeptReading(*epub);

  Position after;
  if (decision_->hasRemotePct) {
    after.hasPct = true;
    after.pctMicro = decision_->remotePct;
  } else {
    after.setPercent(epub->calculateProgress(saved.spine, 0) * 100.0);
  }
  if (decision_->xpointerUsable) strncpy(after.xpointer, decision_->remoteXpointer, kMaxXpointer - 1);

  // The position is on disk: from here on, losing power only means asking again.
  const SessionReport report = stack_->engine.applyChoice(*decision_, ResumeChoice::GoThere, *before_, after, false);
  if (report.outcome == SessionOutcome::Ok) {
    HalFile flag;
    const std::string flagPath = epub->getCachePath() + "/borges_undo";
    if (Storage.openFileForWrite("BORGES", flagPath, flag)) flag.close();
  } else {
    LOG_ERR("BORGES", "Choice could not be stored (%d)", static_cast<int>(report.outcome));
  }
  return true;
}

void BorgesResumeActivity::stayHere() {
  work_ = makeUniqueNoThrow<Work>();
  auto finalized = makeUniqueNoThrow<Position>();
  if (!work_ || !finalized) {
    leave(false);
    return;
  }
  work_->stack = stack_.get();
  work_->decision = decision_.get();
  work_->stayAt = decision_->local;
  const bool hasFinalized = stack_->engine.lastFinalized(decision_->book, *finalized);
  if (chooseStaySource(hasFinalized, *finalized, decision_->local) == StaySource::Ledger) {
    work_->stayAt = *finalized;
  } else {
    // Wi-Fi off before the book loads, as for a jump.
    stack_->bringDown();
    mapSavedPage(work_->stayAt);
  }
  stack_->cancel.arm(kStayBudgetMs);
  workDeadlineAt_ = millis() + kStayBudgetMs + kHangMarginMs;
  if (!worker_.start(&BorgesResumeActivity::stayMain, work_.get())) {
    stack_->engine.applyChoice(*decision_, ResumeChoice::StayHere, work_->stayAt, work_->stayAt, false);
    leave(false);
  }
}

void BorgesResumeActivity::mapSavedPage(Position& out) {
  auto epub = std::make_shared<Epub>(path_, "/.crosspoint");
  epub->setupCacheDir();
  if (!epub->load(true, true, Epub::XLocationLoadMode::Immediate, true)) {
    LOG_ERR("BORGES", "Could not load the book to place this reader");
    return;
  }
  EpubReaderUtils::Progress progress;
  if (!EpubReaderUtils::loadProgress(*epub, progress, "BORGES")) return;  // Nothing saved: no position to send.
  CrossPointPosition local = {progress.spineIndex, progress.pageNumber,
                              progress.hasPageCount ? std::max(1, progress.pageCount) : 1};
  if (progress.hasVisibleTextOffset) {
    local.visibleTextOffset = progress.visibleTextOffset;
    local.hasVisibleTextOffset = true;
  }
  // The same inflate as a jump: on the frame's storage, or the xpointer is lost.
  GfxRenderer::FrameBufferLoan loan(renderer);
  const KOReaderPosition mapped = ProgressMapper::toKOReader(epub, local);
  loan.end();
  if (!mapped.valid) return;
  out.setPercent(mapped.percentage * 100.0);
  out.xpointer[0] = '\0';
  if (mapped.xpath.size() < kMaxXpointer) strncpy(out.xpointer, mapped.xpath.c_str(), kMaxXpointer - 1);
}

void BorgesResumeActivity::goThere() {
  // Wi-Fi off before the book loads: this is the most memory the mapping can have.
  stack_->bringDown();
  if (applyJump()) {
    // The place is on disk and the choice stored. A short try at sending it now, as the
    // plugin does; any button goes on, and the next session sends it otherwise.
    choiceStored_ = true;
    jumped_ = true;
    requestUpdate();  // Now it is true that the place moved: say how to undo it.
    work_ = makeUniqueNoThrow<Work>();
    if (!work_) {
      leave(true);
      return;
    }
    work_->stack = stack_.get();
    work_->decision = decision_.get();
    stack_->cancel.arm(kStayBudgetMs);
    workDeadlineAt_ = millis() + kStayBudgetMs + kHangMarginMs;
    if (!worker_.start(&BorgesResumeActivity::drainMain, work_.get())) leave(true);
    return;
  }
  state_ = State::CouldNotLocate;
  noticeUntil_ = millis() + kNoticeMs;
  requestUpdate(true);
}

void BorgesResumeActivity::leave(const bool success, const bool pressed) {
  if (stack_) stack_->cancel.request();
  if (back_ == Return::Reader) {
    if (worker_.waitForExit(kLeaveWaitMs)) {
      // Whatever came of it, the check itself got its answer: there was a prompt.
      completeSession(success, path_, success && stack_ ? &stack_->clock : nullptr, CheckOutcome::Answered);
    } else if (pressed) {
      noteLeftSessionOnPurpose();
    }
    silentRestartToReader(true);
    return;
  }
  if (!worker_.waitForExit(1000)) {
    silentRestart();
    return;
  }
  if (stack_) stack_->bringDown();
  auto status = makeUniqueNoThrow<BorgesActivity>(renderer, mappedInput, true, NetPayload::Status);
  if (status) {
    activityManager.replaceActivity(std::move(status));
  } else {
    silentRestart();
  }
}

void BorgesResumeActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Rect area = screenArea(renderer);
  int y = area.y + kBlockGap;

  if (state_ == State::Working || state_ == State::CouldNotLocate) {
    y = area.y + area.height / 3;
    y = drawWrappedCentered(renderer, area, UI_12_FONT_ID, y, title_.c_str(), 3, EpdFontFamily::BOLD);
    y += kBlockGap;
    y = drawWrappedCentered(renderer, area, UI_10_FONT_ID, y,
                            state_ == State::Working ? tr(STR_BORGES_OPENING) : tr(STR_BORGES_CANT_LOCATE), 3);
    if (state_ == State::Working && jumped_) {
      drawWrappedCentered(renderer, area, SMALL_FONT_ID, y + kBlockGap, tr(STR_BORGES_JUMP_TIP), 4);
    }
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  const std::string book = renderer.truncatedText(SMALL_FONT_ID, title_.c_str(), area.width);
  renderer.drawText(SMALL_FONT_ID, area.x, y, book.c_str());
  y += renderer.getLineHeight(SMALL_FONT_ID) + kLineGap;
  y = drawWrapped(renderer, UI_12_FONT_ID, area.x, y, area.width, tr(STR_BORGES_RESUME_TITLE), 2, EpdFontFamily::BOLD);
  y += kBlockGap;

  // Two different percentages read at a glance. Only the same whole number needs a decimal.
  const ResumeDecision& d = *decision_;
  const bool sameWhole =
      d.local.hasPct && d.hasRemotePct && (d.local.pctMicro + 500000) / 1000000 == (d.remotePct + 500000) / 1000000;
  char here[16];
  char there[16];
  if (d.local.hasPct) {
    formatPercent(here, sizeof(here), d.local.pctMicro, sameWhole);
  } else {
    snprintf(here, sizeof(here), "%s", tr(STR_BORGES_UNOPENED));
  }
  if (d.hasRemotePct) {
    formatPercent(there, sizeof(there), d.remotePct, sameWhole);
  } else {
    snprintf(there, sizeof(there), "%s", "-");
  }
  y = drawLabelValue(renderer, area.x, y, area.width, tr(STR_BORGES_HERE), here);
  y += kLineGap;
  drawHairline(renderer, area.x, y, area.width);
  y += kLineGap + 2;

  char label[80];
  if (d.remoteDeviceName[0]) {
    snprintf(label, sizeof(label), tr(STR_BORGES_THERE_FMT), d.remoteDeviceName);
  } else {
    snprintf(label, sizeof(label), "%s", tr(STR_BORGES_OTHER_READER));
  }
  y = drawLabelValue(renderer, area.x, y, area.width, label, there);
  if (d.showDate && d.occurredAt[0]) {
    int64_t epoch = 0;
    char age[32] = {0};
    if (parseIsoEpoch(d.occurredAt, epoch)) formatAge(age, sizeof(age), ageOf(epoch, stack_->clock.wallNow()));
    if (age[0]) {
      renderer.drawText(SMALL_FONT_ID, area.x, y, age);
      y += renderer.getLineHeight(SMALL_FONT_ID);
    }
  }
  y += kBlockGap;

  static const StrId kWarnings[] = {StrId::STR_BORGES_WARN_BACKWARDS, StrId::STR_BORGES_WARN_CLOSE,
                                    StrId::STR_BORGES_WARN_APPROXIMATE};
  for (size_t i = 0; i < 3; ++i) {
    if (d.warnings & (1u << i)) {
      y = drawWrapped(renderer, SMALL_FONT_ID, area.x, y, area.width, I18N.get(kWarnings[i]), 3);
      y += kLineGap;
    }
  }

  const int rowHeight = actionRowHeight(renderer);
  const int rowsTop = area.y + area.height - rowHeight * 2 - kActionGap - kBlockGap;
  // The action says where it goes, so nobody has to remember which number was which.
  char go[48];
  char stay[48];
  if (d.hasRemotePct) {
    snprintf(go, sizeof(go), tr(STR_BORGES_GO_FMT), there);
  } else {
    snprintf(go, sizeof(go), "%s", tr(STR_BORGES_GO));
  }
  if (d.local.hasPct) {
    snprintf(stay, sizeof(stay), tr(STR_BORGES_STAY_FMT), here);
  } else {
    snprintf(stay, sizeof(stay), "%s", tr(STR_BORGES_STAY));
  }
  drawActionRow(renderer, area.x, rowsTop, area.width, rowHeight, go, focus_ == 0);
  drawActionRow(renderer, area.x, rowsTop + rowHeight + kActionGap, area.width, rowHeight, stay, focus_ == 1);

  // Back answers nothing: the book opens where it was and this asks again. It says so.
  const auto labels = mappedInput.mapLabels(MappedInputManager::Label(tr(STR_BORGES_LATER)), tr(STR_SELECT),
                                            tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // The screen before it is a line of text on white, so fast is clean enough. A panel will tell.
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

// ---------------------------------------------------------------------------------
// Sleep flush
// ---------------------------------------------------------------------------------

void BorgesSleepFlushActivity::onEnter() {
  Activity::onEnter();
  deadlineAt_ = millis() + kSleepHardDeadlineMs;
  stack_ = makeStack();
  if (!stack_) {
    sleepNow();
    return;
  }
  stack_->cancel.arm(kSleepBudgetMs);
  if (!worker_.start(&BorgesSleepFlushActivity::sessionMain, stack_.get())) sleepNow();
}

void BorgesSleepFlushActivity::sessionMain(void* context) {
  BorgesStack& stack = *static_cast<BorgesStack*>(context);
  if (!stack.openStore() || !stack.openEngine()) return;
  const net::LinkResult link = stack.bringUp(kSleepLinkBudgetMs);
  if (link == net::LinkResult::Connected) {
    stack.engine.runSleepSession();
  } else if (link != net::LinkResult::Cancelled) {
    stack.engine.noteLinkFailure();
  }
  stack.bringDown();
}

void BorgesSleepFlushActivity::loop() {
  if (sleeping_) return;
  if (mappedInput.wasAnyPressed()) {
    wakeNow();
    return;
  }
  if (worker_.finished() || millis() >= deadlineAt_) sleepNow();
}

// A press says the person is back: stop the flush and go where a wake would, not to sleep and
// make them press again.
void BorgesSleepFlushActivity::wakeNow() {
  if (sleeping_) return;
  sleeping_ = true;
  if (stack_) stack_->cancel.request();
  const bool back = worker_.waitForExit(kWakeWaitMs);
  if (back) {
    completeSession(false, {}, nullptr);
    if (stack_) stack_->bringDown();
    stack_.reset();  // The check below builds its own, and this boot's heap is tight.
  } else {
    noteLeftSessionOnPurpose();
  }
  const std::string path = APP_STATE.openEpubPath;
  if (!path.empty() && APP_STATE.lastSleepFromReader) {
    if (back && wakeWantsNetworkBoot(path)) {
      // The guard a wake into a book takes: the reader's clean exit clears it.
      APP_STATE.readerActivityLoadCount++;
      APP_STATE.saveToFile();
      silentRestartToNetwork(NetworkBootTarget::BORGES, static_cast<uint32_t>(NetPayload::OpenBook));
    } else {
      silentRestartToReader(true);
    }
    return;
  }
  silentRestart();
}

void BorgesSleepFlushActivity::sleepNow() {
  if (sleeping_) return;
  sleeping_ = true;
  if (stack_) stack_->cancel.request();
  // Clearing the guard is a courtesy to the next flush. Skip it if the session still holds
  // the card, since nothing may keep sleep from happening.
  if (worker_.waitForExit(500)) completeSession(false, {}, nullptr);
  finishDeepSleepAfterBorgesFlush();
}

}  // namespace glue
}  // namespace borges
