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
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "net/NetFailure.h"
#include "util/QrUtils.h"

namespace borges {
namespace glue {

namespace {

constexpr uint32_t kSyncLinkBudgetMs = 20000;
constexpr uint32_t kPairLinkBudgetMs = 20000;
constexpr uint32_t kManualBudgetMs = 120000;
constexpr uint32_t kDisconnectBudgetMs = 30000;
// Past a budget a session sees through ICancel, the one step it cannot be interrupted in. After
// that it is hung, and the screen leaves without it: every way out of here ends in a reboot.
constexpr unsigned long kHangMarginMs = 15000;
constexpr uint32_t kPairHungMs = 60000;  // Pairing has no budget, so its loop reports it is alive.
constexpr uint32_t kCancelWaitMs = 1500;
// A good result is read in passing and leaves by itself. A bad one stays until a button says it was read.
constexpr unsigned long kSyncOkMs = 1500;
constexpr unsigned long kPairOkMs = 3000;
constexpr int kMaxCodeRenewals = 3;

Rect contentArea(const GfxRenderer& renderer, const int top) {
  Rect area = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int bottom = area.y + area.height;
  area.y = top;
  area.height = bottom - top;
  return area;
}

}  // namespace

struct BorgesActivity::Shared {
  std::unique_ptr<BorgesStack> stack_;
  std::unique_ptr<PairingFlow> pairing_;
  std::unique_ptr<ResumeDecision> decision_;
  std::unique_ptr<BookRef> bookRef_;
  std::unique_ptr<Position> local_;
  std::string bookTitle_;
  std::string bookPath_;
  // Written by the session task, read by the main task.
  std::atomic<View> requestedView_{View::Status};
  std::atomic<bool> viewChanged_{false};
  std::atomic<StrId> resultText_{StrId::STR_NONE_OPT};
  std::atomic<bool> resultOk_{false};
  std::atomic<bool> needsChoice_{false};
  bool askOtherDevices_ = false;
  std::atomic<uint32_t> pairAliveAt_{0};
};

BorgesActivity::BorgesActivity(GfxRenderer& renderer, MappedInputManager& input, const bool networkBoot,
                               const NetPayload start)
    : Activity("Borges", renderer, input), networkBoot_(networkBoot), start_(start) {}

BorgesActivity::~BorgesActivity() = default;

void BorgesActivity::onEnter() {
  Activity::onEnter();
  s_ = makeUniqueNoThrow<Shared>();
  if (!s_) {
    LOG_ERR("BORGES", "OOM: screen state");
    if (networkBoot_) {
      silentRestart();
    } else {
      finish();
    }
    return;
  }
  s_->stack_ = makeStack();
  stackOk_ = s_->stack_ && s_->stack_->openStore() && s_->stack_->openEngine();
  if (start_ == NetPayload::ManualFromReader || start_ == NetPayload::OtherDevice) {
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  }
  buildRows();
  fullPaint_ = true;
  if (networkBoot_ && stackOk_) {
    if (start_ == NetPayload::Pair) {
      startPair();
      return;
    }
    if (start_ == NetPayload::ManualFromHome || start_ == NetPayload::ManualFromReader ||
        start_ == NetPayload::OtherDevice) {
      startSync();
      return;
    }
    if (start_ == NetPayload::Disconnect) {
      startDisconnect();
      return;
    }
  }
  requestUpdate();
}

void BorgesActivity::onExit() {
  if (s_ && s_->stack_) s_->stack_->cancel.request();
  if (!worker_.waitForExit(4000)) {
    // Only on the way to deep sleep, which ends every task.
    (void)s_.release();
  } else if (s_ && s_->stack_) {
    s_->stack_->bringDown();
  }
  Activity::onExit();
}

void BorgesActivity::buildRows() {
  rows_.clear();
  rows_.reserve(6);
  const bool paired = stackOk_ && s_->stack_->store.creds().paired();
  const bool relink = paired && s_->stack_->store.state().needsRelink;
  if (!paired || relink) rows_.push_back(Row::Connect);
  if (paired) {
    rows_.push_back(Row::SyncNow);
    rows_.push_back(Row::Library);
    rows_.push_back(Row::OnOpen);
    rows_.push_back(Row::OnSleep);
    rows_.push_back(Row::Disconnect);
  }
  focus_ = std::min(focus_, std::max(0, static_cast<int>(rows_.size()) - 1));
}

void BorgesActivity::savePrefs() {
  if (!s_->stack_->glueStore.save(s_->stack_->glue)) LOG_ERR("BORGES", "Could not save preferences");
}

void BorgesActivity::setView(const View view) {
  view_ = view;
  // One slow, clean refresh where it earns its time: the list, and a code that is scanned. Text
  // over text repaints fast. A pending first paint (over whatever the boot left) stays pending.
  if (view == View::Status || view == View::PairCode) fullPaint_ = true;
  requestUpdate();
}

// Back to the status list. A reader that has just been linked lands on the library: it is the next thing.
void BorgesActivity::showStatus() {
  {
    RenderLock lock(*this);
    s_->pairing_.reset();
  }
  buildRows();
  if (pairedNow_) {
    for (size_t i = 0; i < rows_.size(); ++i) {
      if (rows_[i] == Row::Library) focus_ = static_cast<int>(i);
    }
    pairedNow_ = false;
  }
  setView(View::Status);
}

// Asks the session to stop and says so at once: the wait for it can take a second or two.
void BorgesActivity::cancelWork() {
  s_->stack_->cancel.request();
  cancelling_ = true;
  (void)requestUpdateAndWait();
}

void BorgesActivity::activate(const Row row) {
  switch (row) {
    case Row::Connect:
      if (networkBoot_) {
        startPair();
      } else {
        activityManager.replaceActivity(makeHandoff(renderer, mappedInput, NetPayload::Pair));
      }
      return;
    case Row::SyncNow:
      if (networkBoot_) {
        startSync();
      } else {
        activityManager.replaceActivity(makeHandoff(renderer, mappedInput, NetPayload::ManualFromHome));
      }
      return;
    case Row::Library:
      if (networkBoot_) {
        {
          RenderLock lock(*this);
          s_->stack_->bringDown();
          s_->stack_.reset();
          stackOk_ = false;
        }
        auto library = makeUniqueNoThrow<BorgesLibraryActivity>(renderer, mappedInput);
        if (library) {
          activityManager.replaceActivity(std::move(library));
        } else {
          silentRestart();
        }
      } else {
        activityManager.replaceActivity(makeHandoff(renderer, mappedInput, NetPayload::Library));
      }
      return;
    case Row::OnOpen:
      s_->stack_->glue.autoOnOpen = !s_->stack_->glue.autoOnOpen;
      savePrefs();
      requestUpdate();
      return;
    case Row::OnSleep:
      s_->stack_->glue.syncOnSleep = !s_->stack_->glue.syncOnSleep;
      savePrefs();
      requestUpdate();
      return;
    case Row::Disconnect:
      startActivityForResult(
          // The dialog runs the two together as one sentence, so the first ends in its own question mark.
          std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_BORGES_DISCONNECT_ASK),
                                                 tr(STR_BORGES_DISCONNECT_CONFIRM)),
          [this](const ActivityResult& result) {
            fullPaint_ = true;
            if (result.isCancelled) return;
            if (networkBoot_) {
              startDisconnect();
            } else {
              activityManager.replaceActivity(makeHandoff(renderer, mappedInput, NetPayload::Disconnect));
            }
          });
      return;
  }
}

// ---------------------------------------------------------------------------------
// Manual sync
// ---------------------------------------------------------------------------------

void BorgesActivity::startSync() {
  if (!hasSavedWifi() && !s_->stack_->link.connected()) {
    // First network ever. The picker owns the connection and the password.
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, true, true),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               setView(View::Status);
                             } else {
                               startSync();
                             }
                           });
    return;
  }
  s_->stack_->cancel.arm(kManualBudgetMs);
  s_->askOtherDevices_ = start_ == NetPayload::OtherDevice;
  hardDeadlineAt_ = millis() + kManualBudgetMs + kHangMarginMs;
  if ((start_ == NetPayload::ManualFromReader || start_ == NetPayload::OtherDevice) &&
      !APP_STATE.openEpubPath.empty()) {
    s_->bookPath_ = APP_STATE.openEpubPath;
    s_->bookTitle_ = bookTitleFor(s_->bookPath_);
    s_->bookRef_ = makeUniqueNoThrow<BookRef>();
    s_->local_ = makeUniqueNoThrow<Position>();
    s_->decision_ = makeUniqueNoThrow<ResumeDecision>();
  }
  // With the picker's connection already up there is nothing to connect to.
  setView(s_->stack_->link.connected() ? View::Syncing : View::Connecting);
  if (!worker_.start(&BorgesActivity::syncMain, s_.get())) {
    s_->resultText_.store(StrId::STR_BORGES_ERR_LOCAL);
    s_->resultOk_.store(false);
    autoLeaveAt_ = 0;
    setView(View::SyncResult);
  }
}

void BorgesActivity::syncMain(void* shared) {
  auto* s = static_cast<BorgesActivity::Shared*>(shared);
  BorgesStack& stack = *s->stack_;
  const auto finish = [s](const StrId text, const bool ok) {
    s->resultText_.store(text);
    s->resultOk_.store(ok);
    s->requestedView_.store(View::SyncResult);
    s->viewChanged_.store(true, std::memory_order_release);
  };

  // A person pressing sync is what lets a reader left in a drawer accept a much later time.
  stack.clock.allowLongGap();
  const net::LinkResult link = stack.bringUp(kSyncLinkBudgetMs);
  if (link != net::LinkResult::Connected) {
    finish(describeLink(link), false);
    return;
  }
  s->requestedView_.store(View::Syncing);
  s->viewChanged_.store(true, std::memory_order_release);

  const bool withBook =
      s->bookRef_ && s->local_ && s->decision_ && partialMd5(stack.fs, s->bookPath_.c_str(), s->bookRef_->id);
  if (withBook) {
    s->bookRef_->title = s->bookTitle_.c_str();
    const float percent = localPercentFor(s->bookPath_);
    if (percent >= 0) s->local_->setPercent(percent);
  }
  const bool askOthers = withBook && s->askOtherDevices_;
  const SessionReport report =
      askOthers  ? stack.engine.runPositionsSession(*s->bookRef_, *s->local_, *s->decision_)
      : withBook ? stack.engine.runManualSession(s->bookRef_.get(), s->local_.get(), s->decision_.get())
                 : stack.engine.runManualSession(nullptr, nullptr, nullptr);
  if (report.outcome == SessionOutcome::AwaitingChoice) {
    s->needsChoice_.store(true);
    s->viewChanged_.store(true, std::memory_order_release);
    return;
  }
  stack.bringDown();
  if (askOthers && report.outcome == SessionOutcome::Ok) {
    finish(StrId::STR_BORGES_NO_OTHER_DEVICE, false);
    return;
  }
  // The engine's word for a lost connection is general. The diagnostics know which one it was.
  finish(report.outcome == SessionOutcome::Transport ? describeDiagnostics(stack.store.state().diag)
                                                     : describeOutcome(report.outcome),
         report.outcome == SessionOutcome::Ok);
}

// ---------------------------------------------------------------------------------
// Disconnect
// ---------------------------------------------------------------------------------

void BorgesActivity::startDisconnect() {
  s_->stack_->cancel.arm(kDisconnectBudgetMs);
  hardDeadlineAt_ = millis() + kDisconnectBudgetMs + kHangMarginMs;
  focus_ = 0;
  setView(View::Unlinking);
  if (!worker_.start(&BorgesActivity::disconnectMain, s_.get())) {
    s_->stack_->engine.disconnect(false);
    s_->resultText_.store(StrId::STR_BORGES_DISCONNECT_PENDING);
    s_->resultOk_.store(false);
    autoLeaveAt_ = 0;
    setView(View::SyncResult);
  }
}

void BorgesActivity::disconnectMain(void* shared) {
  auto* s = static_cast<BorgesActivity::Shared*>(shared);
  BorgesStack& stack = *s->stack_;
  const bool online = stack.bringUp(kSyncLinkBudgetMs) == net::LinkResult::Connected;
  const DisconnectResult result = stack.engine.disconnect(online);
  stack.bringDown();
  s->resultText_.store(result == DisconnectResult::ServerInvalidated    ? StrId::STR_BORGES_DISCONNECT_DONE
                       : result == DisconnectResult::StillValidOnServer ? StrId::STR_BORGES_DISCONNECT_PENDING
                                                                        : StrId::STR_BORGES_ERR_LOCAL);
  s->resultOk_.store(result == DisconnectResult::ServerInvalidated);
  s->requestedView_.store(View::SyncResult);
  s->viewChanged_.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------------
// Pairing
// ---------------------------------------------------------------------------------

void BorgesActivity::startPair() {
  if (!hasSavedWifi() && !s_->stack_->link.connected()) {
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, true, true),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               setView(View::Status);
                             } else {
                               beginPairSession();
                             }
                           });
    return;
  }
  beginPairSession();
}

void BorgesActivity::beginPairSession() {
  PairingConfig config;
  config.externalId = s_->stack_->externalId;
  // The account lists devices by this name. "CrossInk X4" says less than "X4".
  s_->bookTitle_ = SETTINGS.getEffectiveDeviceName();
  constexpr char kPrefix[] = "CrossInk ";
  if (s_->bookTitle_.rfind(kPrefix, 0) == 0) s_->bookTitle_.erase(0, sizeof(kPrefix) - 1);
  config.deviceName = s_->bookTitle_.c_str();
  config.firmwareVersion = s_->stack_->clientVersion + strlen("crossink-borges/");
  config.clientVersion = s_->stack_->clientVersion;
  s_->pairing_ = makeUniqueNoThrow<PairingFlow>(s_->stack_->store, s_->stack_->outbox, s_->stack_->fs, s_->stack_->http,
                                                s_->stack_->clock, s_->stack_->random, s_->stack_->cancel, config);
  if (!s_->pairing_) {
    s_->resultText_.store(StrId::STR_BORGES_ERR_LOCAL);
    s_->resultOk_.store(false);
    setView(View::PairResult);
    return;
  }
  s_->stack_->cancel.arm(0);
  hardDeadlineAt_ = 0;
  s_->pairAliveAt_.store(millis());
  setView(View::Connecting);
  if (!worker_.start(&BorgesActivity::pairMain, s_.get())) {
    s_->resultText_.store(StrId::STR_BORGES_ERR_LOCAL);
    s_->resultOk_.store(false);
    setView(View::PairResult);
  }
}

void BorgesActivity::pairMain(void* shared) {
  auto* s = static_cast<BorgesActivity::Shared*>(shared);
  BorgesStack& stack = *s->stack_;
  PairingFlow& flow = *s->pairing_;
  const auto show = [s](const View view) {
    s->requestedView_.store(view);
    s->viewChanged_.store(true, std::memory_order_release);
  };
  const auto finish = [&](const StrId text, const bool ok) {
    stack.bringDown();
    s->resultText_.store(text);
    s->resultOk_.store(ok);
    show(View::PairResult);
  };

  const net::LinkResult link = stack.bringUp(kPairLinkBudgetMs);
  if (link != net::LinkResult::Connected) {
    finish(describeLink(link), false);
    return;
  }

  int renewals = 0;
  bool started = false;
  while (!stack.cancel.cancelled()) {
    s->pairAliveAt_.store(millis());
    if (!started) {
      if (flow.start()) {
        started = true;
        show(View::PairCode);
      } else if (!flow.rateLimited()) {
        finish(StrId::STR_BORGES_PAIR_FAILED, false);
        return;
      } else {
        delay(5000);
      }
      continue;
    }
    switch (flow.step()) {
      case PairingState::Done:
        // The reader is linked and the radio is up: the first sync (and the library behind it) costs
        // nothing more now than later. Its own failure is not the link's: the status screen says it.
        stack.cancel.arm(kManualBudgetMs);
        stack.clock.allowLongGap();
        show(View::Syncing);
        (void)stack.engine.runManualSession(nullptr, nullptr, nullptr);
        finish(StrId::STR_BORGES_PAIR_DONE, true);
        return;
      case PairingState::Rejected:
        finish(StrId::STR_BORGES_PAIR_REJECTED, false);
        return;
      case PairingState::Failed:
        finish(StrId::STR_BORGES_PAIR_FAILED, false);
        return;
      case PairingState::Expired:
      case PairingState::ClaimLost:
        // A code ran out while the phone was in a pocket. A new one, without asking.
        if (++renewals > kMaxCodeRenewals) {
          finish(StrId::STR_BORGES_PAIR_EXPIRED, false);
          return;
        }
        started = false;
        break;
      case PairingState::Idle:
      case PairingState::WaitingForApproval:
        delay(250);
        break;
    }
  }
  stack.bringDown();
}

// ---------------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------------

void BorgesActivity::leave() {
  if (s_->stack_) s_->stack_->cancel.request();
  (void)worker_.waitForExit(kCancelWaitMs);
  if (networkBoot_) {
    if (readerSession()) {
      silentRestartToReader(true);
    } else {
      silentRestart();
    }
    return;
  }
  finish();
}

void BorgesActivity::loop() {
  if (!s_) return;  // Could not allocate it: already on the way out.
  if (s_->viewChanged_.exchange(false, std::memory_order_acquire)) {
    if (s_->needsChoice_.exchange(false)) {
      RenderLock lock(*this);
      stackOk_ = false;
      auto resume = makeUniqueNoThrow<BorgesResumeActivity>(
          renderer, mappedInput, std::move(s_->stack_), std::move(s_->decision_), s_->bookPath_, s_->bookTitle_,
          readerSession() ? BorgesResumeActivity::Return::Reader : BorgesResumeActivity::Return::Status);
      if (resume) {
        activityManager.replaceActivity(std::move(resume));
      } else {
        leave();
      }
      return;
    }
    const View next = s_->requestedView_.load();
    if (next == View::Syncing && view_ == View::Syncing) return;  // Already says so.
    if (next == View::Syncing && hardDeadlineAt_ == 0) {
      // The first sync after pairing: a session like any other from here.
      hardDeadlineAt_ = millis() + kManualBudgetMs + kHangMarginMs;
    }
    if (next == View::SyncResult || next == View::PairResult) {
      const bool ok = s_->resultOk_.load();
      autoLeaveAt_ = ok ? millis() + (next == View::PairResult ? kPairOkMs : kSyncOkMs) : 0;
      if (next == View::PairResult && ok) pairedNow_ = true;
      const Diagnostics& diagnostics = s_->stack_->store.state().diag;
      snprintf(requestId_, sizeof(requestId_), "%s", ok ? "" : diagnostics.requestId);
    }
    setView(next);
    return;
  }

  switch (view_) {
    case View::Connecting:
    case View::Syncing:
    case View::Unlinking:
    case View::PairCode: {
      // A session with a budget has a deadline. Pairing has none, so its loop reports it is alive.
      const bool deadlineHung = hardDeadlineAt_ != 0 && millis() >= hardDeadlineAt_;
      const bool pairHung = hardDeadlineAt_ == 0 && millis() - s_->pairAliveAt_.load() >= kPairHungMs;
      if (deadlineHung || pairHung) {
        LOG_ERR("BORGES", "Session hung: leaving without it");
        leave();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        cancelWork();
        if (!worker_.waitForExit(kCancelWaitMs)) {
          leave();
          return;
        }
        cancelling_ = false;
        s_->stack_->bringDown();
        if (readerSession()) {
          leave();
        } else {
          showStatus();
        }
      }
      return;
    }
    case View::SyncResult:
    case View::PairResult:
      if (mappedInput.wasAnyPressed() || (autoLeaveAt_ != 0 && millis() >= autoLeaveAt_)) {
        if (readerSession()) {
          leave();
          return;
        }
        showStatus();
      }
      return;
    case View::Status:
      break;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    leave();
    return;
  }
  if (!rows_.empty() && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate(rows_[focus_]);
    return;
  }
  const int count = static_cast<int>(rows_.size());
  if (count > 1) {
    navigator_.onNextRelease([this, count] {
      focus_ = ButtonNavigator::nextIndex(focus_, count);
      requestUpdate();
    });
    navigator_.onPreviousRelease([this, count] {
      focus_ = ButtonNavigator::previousIndex(focus_, count);
      requestUpdate();
    });
  }
}

// ---------------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------------

void BorgesActivity::renderCentered(const char* primary, const char* secondary) {
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  Rect area = screen;
  area.x += kSideMargin;
  area.width -= kSideMargin * 2;
  int y = area.y + area.height / 3;
  y = drawWrappedCentered(renderer, area, UI_12_FONT_ID, y, primary, 3, EpdFontFamily::BOLD);
  if (secondary && secondary[0]) {
    y += kBlockGap;
    drawWrappedCentered(renderer, area, SMALL_FONT_ID, y, secondary, 2);
  }
}

// What a session said. A failure carries the reference a report quotes, and where to make one.
void BorgesActivity::renderResult() {
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  Rect area = screen;
  area.x += kSideMargin;
  area.width -= kSideMargin * 2;
  int y = area.y + area.height / 3;
  y = drawWrappedCentered(renderer, area, UI_12_FONT_ID, y, I18N.get(s_->resultText_.load()), 5, EpdFontFamily::BOLD);
  if (requestId_[0]) {
    char id[64];
    snprintf(id, sizeof(id), tr(STR_BORGES_REQUEST_ID_FMT), requestId_);
    y += kBlockGap;
    y = drawWrappedCentered(renderer, area, SMALL_FONT_ID, y, id, 2);
    drawWrappedCentered(renderer, area, SMALL_FONT_ID, y + kLineGap, tr(STR_BORGES_REPORT), 2);
  }
}

void BorgesActivity::renderPairCode() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int headerBottom = metrics.topPadding + metrics.headerHeight;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_BORGES_CONNECT));

  Rect area = contentArea(renderer, headerBottom);
  area.x += kSideMargin;
  area.width -= kSideMargin * 2;

  // The URL is what a person types when a QR will not scan.
  std::string url = s_->pairing_->qrUrl();
  const size_t scheme = url.find("://");
  std::string shown = url;
  if (scheme != std::string::npos) shown.erase(0, scheme + 3);
  const size_t query = shown.find('?');
  if (query != std::string::npos) shown.resize(query);
  char line[200];
  snprintf(line, sizeof(line), tr(STR_BORGES_PAIR_OPEN_FMT), shown.c_str());

#if CROSSINK_SCALABLE_FONTS
  constexpr int kCodeFont = UI_12_FONT_ID;
#else
  constexpr int kCodeFont = LEXENDDECA_16_FONT_ID;
#endif
  constexpr int kCodeGap = 6;

  // What goes under the code, measured first so the QR takes what is left and not a pixel more.
  const int instructionHeight =
      static_cast<int>(renderer.wrappedText(SMALL_FONT_ID, line, area.width, 3, EpdFontFamily::REGULAR).size()) *
      renderer.getLineHeight(SMALL_FONT_ID);
  const int footerHeight =
      static_cast<int>(
          renderer.wrappedText(SMALL_FONT_ID, tr(STR_BORGES_PAIR_WAITING), area.width, 2, EpdFontFamily::REGULAR)
              .size()) *
      renderer.getLineHeight(SMALL_FONT_ID);
  const int codeHeight = renderer.getLineHeight(kCodeFont);
  const int below = codeHeight + kLineGap + instructionHeight + kBlockGap + footerHeight;

  // A scanner wants four modules of white all round. A version 4 symbol is 33 modules; 11 px each reads
  // from arm's length on this panel, and a smaller module only when the screen has no room for it.
  constexpr int kModules = 33;
  constexpr int kQuietModules = 4;
  const bool fitsVersion4 = url.size() <= 114;
  int module = 11;
  const int room = area.height - below;
  while (module > 4 && (kModules + 2 * kQuietModules) * module > room) --module;
  const int quiet = kQuietModules * module;
  const int qr = fitsVersion4 ? kModules * module : std::min(area.width, room - 2 * quiet);

  int y = area.y + quiet;
  if (qr > 96) {
    QrUtils::drawQrCode(renderer, Rect{area.x + (area.width - qr) / 2, y, qr, qr}, url);
  }
  y += qr + quiet;

  // Wide-set: read aloud or typed, a character at a time.
  const char* code = s_->pairing_->userCode();
  int codeWidth = 0;
  char glyph[2] = {0, 0};
  for (const char* p = code; *p; ++p) {
    glyph[0] = *p;
    codeWidth += renderer.getTextWidth(kCodeFont, glyph, EpdFontFamily::BOLD) + kCodeGap;
  }
  int codeX = area.x + (area.width - codeWidth + kCodeGap) / 2;
  for (const char* p = code; *p; ++p) {
    glyph[0] = *p;
    renderer.drawText(kCodeFont, codeX, y, glyph, true, EpdFontFamily::BOLD);
    codeX += renderer.getTextWidth(kCodeFont, glyph, EpdFontFamily::BOLD) + kCodeGap;
  }
  y += codeHeight + kLineGap;

  y = drawWrappedCentered(renderer, area, SMALL_FONT_ID, y, line, 3);
  drawWrappedCentered(renderer, area, SMALL_FONT_ID, y + kBlockGap, tr(STR_BORGES_PAIR_WAITING), 2);
}

void BorgesActivity::renderStatus() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_BORGES));
  int y = metrics.topPadding + metrics.headerHeight + kBlockGap;
  const int x = kSideMargin;
  const int width = renderer.getScreenWidth() - kSideMargin * 2;

  const bool paired = stackOk_ && s_->stack_->store.creds().paired();
  if (!paired) {
    y = drawWrapped(renderer, UI_12_FONT_ID, x, y, width, tr(STR_BORGES_NOT_CONNECTED), 2, EpdFontFamily::BOLD);
    y += kLineGap;
    y = drawWrapped(renderer, UI_10_FONT_ID, x, y, width, tr(STR_BORGES_NOT_CONNECTED_HINT), 4);
    y += kBlockGap;
  } else {
    const EngineStatus status = s_->stack_->engine.status();
    const Credentials& creds = s_->stack_->store.creds();
    char value[48];

    // Who it is linked to, in the words the person knows: never an id.
    if (creds.accountName[0]) {
      snprintf(value, sizeof(value), tr(STR_BORGES_CONNECTED_AS), creds.accountName);
      y = drawWrapped(renderer, UI_10_FONT_ID, x, y, width, value, 1, EpdFontFamily::BOLD) + kLineGap;
    }

    if (status.lastSuccessEpoch <= 0) {
      snprintf(value, sizeof(value), "%s", tr(STR_BORGES_NEVER));
    } else {
      formatAge(value, sizeof(value), ageOf(status.lastSuccessEpoch, s_->stack_->clock.wallNow()));
      if (!value[0]) snprintf(value, sizeof(value), "%s", tr(STR_BORGES_A_WHILE_AGO));
    }
    y = drawLabelValue(renderer, x, y, width, tr(STR_BORGES_LAST_SYNC), value) + kLineGap;

    if (status.depth == 0) {
      snprintf(value, sizeof(value), "%s", tr(STR_BORGES_NOTHING_PENDING));
    } else if (status.depth == 1) {
      snprintf(value, sizeof(value), "%s", tr(STR_BORGES_PENDING_ONE));
    } else {
      snprintf(value, sizeof(value), tr(STR_BORGES_PENDING_FMT), static_cast<unsigned>(status.depth));
    }
    y = drawLabelValue(renderer, x, y, width, tr(STR_BORGES_PENDING), value) + kBlockGap;

    if (status.needsRelink) {
      y = drawWrapped(renderer, UI_10_FONT_ID, x, y, width, tr(STR_BORGES_ERR_RELINK), 2, EpdFontFamily::BOLD);
      y += kBlockGap;
    } else if (status.diag.hasError()) {
      y = drawWrapped(renderer, SMALL_FONT_ID, x, y, width, tr(STR_BORGES_LAST_ERROR), 1, EpdFontFamily::BOLD);
      y = drawWrapped(renderer, UI_10_FONT_ID, x, y, width, I18N.get(describeDiagnostics(status.diag)), 2);
      // Literal, for a report: the words above group several failures.
      if (const char* failure =
              net::failureCode(status.diag.transportStage, status.diag.transportCode, TimePrecision::Unknown)) {
        y = drawWrapped(renderer, SMALL_FONT_ID, x, y, width, failure, 1);
      }
      if (status.diag.requestId[0]) {
        snprintf(value, sizeof(value), tr(STR_BORGES_REQUEST_ID_FMT), status.diag.requestId);
        y = drawWrapped(renderer, SMALL_FONT_ID, x, y, width, value, 2);
        y = drawWrapped(renderer, SMALL_FONT_ID, x, y, width, tr(STR_BORGES_REPORT), 2);
      }
      y += kBlockGap;
    }
  }

  drawHairline(renderer, 0, y - kLineGap, renderer.getScreenWidth());
  Rect list = contentArea(renderer, y);
  const auto labelOf = [this](const int index) -> std::string {
    switch (rows_[index]) {
      case Row::Connect:
        return tr(STR_BORGES_CONNECT);
      case Row::SyncNow:
        return tr(STR_BORGES_SYNC_NOW);
      case Row::Library:
        return tr(STR_BORGES_DOWNLOAD_BOOKS);
      case Row::OnOpen:
        return tr(STR_BORGES_ON_OPEN);
      case Row::OnSleep:
        return tr(STR_BORGES_ON_SLEEP);
      case Row::Disconnect:
        return tr(STR_BORGES_DISCONNECT);
    }
    return {};
  };
  const auto valueOf = [this](const int index) -> std::string {
    if (!stackOk_) return {};
    if (rows_[index] == Row::OnOpen) return s_->stack_->glue.autoOnOpen ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    if (rows_[index] == Row::OnSleep) return s_->stack_->glue.syncOnSleep ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    return {};
  };
  GUI.drawList(renderer, list, static_cast<int>(rows_.size()), focus_, labelOf, nullptr, nullptr, valueOf);

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), rows_.empty() ? "" : tr(STR_SELECT),
                            rows_.size() > 1 ? tr(STR_DIR_UP) : "", rows_.size() > 1 ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BorgesActivity::render(RenderLock&&) {
  if (!s_ || (view_ == View::Status && !stackOk_ && !s_->stack_)) return;  // On the way out, or handed over.
  renderer.clearScreen();
  if (cancelling_) {
    renderCentered(tr(STR_BORGES_CANCELLING), nullptr);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  switch (view_) {
    case View::Status:
      renderStatus();
      break;
    case View::Connecting:
      renderCentered(tr(STR_BORGES_CONNECTING_WIFI), nullptr);
      break;
    case View::Syncing:
      renderCentered(tr(STR_BORGES_SYNCING), nullptr);
      break;
    case View::Unlinking:
      renderCentered(tr(STR_BORGES_UNLINKING), nullptr);
      break;
    case View::PairCode:
      renderPairCode();
      break;
    case View::SyncResult:
    case View::PairResult:
      renderResult();
      break;
  }
  if (view_ == View::Connecting || view_ == View::Syncing || view_ == View::Unlinking || view_ == View::PairCode) {
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if ((view_ == View::SyncResult || view_ == View::PairResult) && !s_->resultOk_.load()) {
    // A failure waits to be read. The hint says what the button does.
    const auto labels = mappedInput.mapLabels("", tr(STR_BORGES_CONTINUE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer(fullPaint_ ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  fullPaint_ = false;
}

}  // namespace glue
}  // namespace borges
