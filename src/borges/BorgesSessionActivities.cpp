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
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "SilentRestart.h"
#include "activities/home/RecentBookProgress.h"
#include "activities/reader/EpubReaderUtils.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace borges {
namespace glue {

namespace {

constexpr uint32_t kOpenBudgetMs = 12000;
constexpr uint32_t kOpenLinkBudgetMs = 9000;
constexpr uint32_t kOpenHardDeadlineMs = kOpenBudgetMs + 2000;
constexpr uint32_t kStayBudgetMs = 5000;
constexpr uint32_t kChoiceLinkBudgetMs = 3000;
constexpr uint32_t kSleepBudgetMs = 15000;
constexpr uint32_t kSleepLinkBudgetMs = 6000;
// One uninterruptible TCP connect, about 3.5 s, on top of the budget.
constexpr uint32_t kSleepHardDeadlineMs = kSleepBudgetMs + 4000;
constexpr uint32_t kNoticeMs = 1500;
constexpr int kActionGap = 8;

// A session came back: the guard's count starts over. Takes its own file handles, since
// the session task may still own the stack's.
void completeSession(const bool openSucceeded, const std::string& path, IClock* clock) {
  net::BorgesFsHal fs;
  GlueStore store(fs);
  GlueState state;
  store.load(state);
  finishSession(state);
  if (openSucceeded && clock) noteOpenSuccess(state, pathHash(path.c_str()), clock->wallNow());
  store.save(state);
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
  BookRef ref;
  Position local;
  ResumeDecision decision;
  SessionReport report;
  bool ran = false;
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
    openBook();
    return;
  }
  stack_->cancel.arm(kOpenBudgetMs);
  requestUpdate(true);
  if (!worker_.start(&BorgesOpenActivity::sessionMain, this)) openBook();
}

void BorgesOpenActivity::sessionMain(void* self) {
  auto* activity = static_cast<BorgesOpenActivity*>(self);
  BorgesStack& stack = *activity->stack_;
  Session& session = *activity->session_;
  if (!stack.openStore() || !stack.openEngine()) return;
  if (!partialMd5(stack.fs, activity->path_.c_str(), session.ref.id)) {
    LOG_ERR("BORGES", "No identity for %s: opening without a check", activity->path_.c_str());
    return;
  }
  session.ref.title = activity->title_.c_str();
  const float percent = localPercentFor(activity->path_);
  if (percent >= 0) session.local.setPercent(percent);

  if (stack.bringUp(kOpenLinkBudgetMs) != net::LinkResult::Connected) return;
  session.report = stack.engine.runOpenSession(session.ref, session.local, session.decision);
  session.ran = true;
  if (session.report.outcome == SessionOutcome::AwaitingChoice) {
    activity->awaitingChoice_.store(true, std::memory_order_release);
  } else {
    stack.bringDown();
  }
}

  // Also here: this boot may have been reached by a path that did not go through routeBook.
  if (!stack.engine.onBookOpened(session.ref.id)) LOG_ERR("BORGES", "Could not hold the book");
void BorgesOpenActivity::loop() {
  const unsigned long now = millis();
  (void)now;
  if (mappedInput.wasAnyPressed()) {
    openBook();
    return;
  }
  if (worker_.finished()) {
    if (awaitingChoice_.load(std::memory_order_acquire)) {
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
    openBook();
    return;
  }
  if (stack_ && stack_->cancel.remainingMs() == 0) {
    // The deadline reached the session through ICancel. Give it one uninterruptible step
    // to come back, then stop waiting: a reader never waits on a socket.
    if (!worker_.waitForExit(kOpenHardDeadlineMs - kOpenBudgetMs)) openBook();
  }
}

void BorgesOpenActivity::openBook() {
  if (stack_) stack_->cancel.request();
  const bool ok = worker_.finished() && session_ && session_->ran && session_->report.outcome == SessionOutcome::Ok;
  completeSession(ok, path_, ok ? &stack_->clock : nullptr);
  if (worker_.waitForExit(400) && stack_) stack_->bringDown();
  // A reboot turns the radio off and frees everything, wherever the session stands.
  silentRestartToReader(true);
}

void BorgesOpenActivity::onExit() {
  if (stack_) stack_->cancel.request();
  if (!worker_.waitForExit(4000)) {
    // Only reachable on the way to deep sleep, which ends every task within moments.
    stack_.release();
    session_.release();
  }
  Activity::onExit();
}

void BorgesOpenActivity::render(RenderLock&&) {
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
  requestUpdate();
}

void BorgesResumeActivity::onExit() {
  if (stack_) stack_->cancel.request();
  if (!worker_.waitForExit(4000)) stack_.release();  // Only on the way to deep sleep.
  Activity::onExit();
}

void BorgesResumeActivity::stayMain(void* self) {
  auto* activity = static_cast<BorgesResumeActivity*>(self);
  BorgesStack& stack = *activity->stack_;
  const Position& here = *activity->stayAt_;
  // The radio may have gone down while the prompt waited, or to map the page. Once more,
  // briefly: this device becoming the head is what the reader just asked for. Without a
  // link the choice is still stored, and the next session sends it.
  const bool online = stack.bringUp(kChoiceLinkBudgetMs) == net::LinkResult::Connected;
  stack.engine.applyChoice(*activity->decision_, ResumeChoice::StayHere, here, here, online);
  stack.bringDown();
}

void BorgesResumeActivity::drainMain(void* self) {
  BorgesStack& stack = *static_cast<BorgesResumeActivity*>(self)->stack_;
  if (stack.bringUp(kChoiceLinkBudgetMs) == net::LinkResult::Connected) stack.engine.runSleepSession();
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
    case State::Working:
      if (mappedInput.wasAnyPressed() || worker_.finished()) leave(choiceStored_ || worker_.finished());
      return;
    case State::Choosing:
      break;
  }

  // Back answers nothing: the book opens where it was, and this asks again next time.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    stack_->engine.abandonChoice();
    leave(false);
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
  const CrossPointPosition mapped =
      ProgressMapper::toCrossPoint(epub, target, hasLocal ? progress.spineIndex : -1, pageCount);
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

void BorgesResumeActivity::goThere() {
  // Wi-Fi off before the book loads: this is the most memory the mapping can have.
  stack_->bringDown();
  if (applyJump()) {
    // The place is on disk and the choice stored. A short try at sending it now, as the
    // plugin does; any button goes on, and the next session sends it otherwise.
    choiceStored_ = true;
    stack_->cancel.arm(kStayBudgetMs);
    if (!worker_.start(&BorgesResumeActivity::drainMain, this)) leave(true);
    return;
  }
  state_ = State::CouldNotLocate;
  noticeUntil_ = millis() + kNoticeMs;
  requestUpdate(true);
}

void BorgesResumeActivity::leave(const bool success) {
  if (stack_) stack_->cancel.request();
  if (back_ == Return::Reader) {
    completeSession(success, path_, success && stack_ ? &stack_->clock : nullptr);
    silentRestartToReader(true);
    return;
  }
  if (!worker_.waitForExit(1000)) {
    silentRestart();
void BorgesResumeActivity::stayHere() {
  stayAt_ = makeUniqueNoThrow<Position>(decision_->local);
  auto finalized = makeUniqueNoThrow<Position>();
  if (!stayAt_ || !finalized) {
    leave(false);
    return;
  }
  const bool hasFinalized = stack_->engine.lastFinalized(decision_->book, *finalized);
  if (chooseStaySource(hasFinalized, *finalized, decision_->local) == StaySource::Ledger) {
    *stayAt_ = *finalized;
  } else {
    // Wi-Fi off before the book loads, as for a jump.
    stack_->bringDown();
    mapSavedPage(*stayAt_);
  }
  stack_->cancel.arm(kStayBudgetMs);
  if (!worker_.start(&BorgesResumeActivity::stayMain, this)) {
    stack_->engine.applyChoice(*decision_, ResumeChoice::StayHere, *stayAt_, *stayAt_, false);
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
  const KOReaderPosition mapped = ProgressMapper::toKOReader(epub, local);
  if (!mapped.valid) return;
  out.setPercent(mapped.percentage * 100.0);
  out.xpointer[0] = '\0';
  if (mapped.xpath.size() < kMaxXpointer) strncpy(out.xpointer, mapped.xpath.c_str(), kMaxXpointer - 1);
}

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
    drawWrappedCentered(renderer, area, UI_10_FONT_ID, y,
                        state_ == State::Working ? tr(STR_BORGES_OPENING) : tr(STR_BORGES_CANT_LOCATE), 3);
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
    snprintf(here, sizeof(here), "%s", "-");
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
  snprintf(label, sizeof(label), tr(STR_BORGES_THERE_FMT), d.remoteDeviceName[0] ? d.remoteDeviceName : "?");
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
  drawActionRow(renderer, area.x, rowsTop, area.width, rowHeight, tr(STR_BORGES_GO), focus_ == 0);
  drawActionRow(renderer, area.x, rowsTop + rowHeight + kActionGap, area.width, rowHeight, tr(STR_BORGES_STAY),
                focus_ == 1);

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(firstPaint_ ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  firstPaint_ = false;
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
  if (!worker_.start(&BorgesSleepFlushActivity::sessionMain, this)) sleepNow();
}

void BorgesSleepFlushActivity::sessionMain(void* self) {
  auto* activity = static_cast<BorgesSleepFlushActivity*>(self);
  BorgesStack& stack = *activity->stack_;
  if (!stack.openStore() || !stack.openEngine()) return;
  if (stack.bringUp(kSleepLinkBudgetMs) == net::LinkResult::Connected) stack.engine.runSleepSession();
  stack.bringDown();
}

void BorgesSleepFlushActivity::loop() {
  if (sleeping_) return;
  if (worker_.finished() || mappedInput.wasAnyPressed() || millis() >= deadlineAt_) sleepNow();
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
