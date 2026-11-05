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
#include "util/QrUtils.h"

namespace borges {
namespace glue {

namespace {

constexpr uint32_t kSyncLinkBudgetMs = 20000;
constexpr uint32_t kPairLinkBudgetMs = 20000;
constexpr uint32_t kManualBudgetMs = 120000;
constexpr unsigned long kResultOkMs = 1500;
constexpr unsigned long kResultErrorMs = 3000;
constexpr int kMaxCodeRenewals = 3;

Rect contentArea(const GfxRenderer& renderer, const int top) {
  Rect area = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int bottom = area.y + area.height;
  area.y = top;
  area.height = bottom - top;
  return area;
}

}  // namespace

BorgesActivity::BorgesActivity(GfxRenderer& renderer, MappedInputManager& input, const bool networkBoot,
                               const NetPayload start)
    : Activity("Borges", renderer, input), networkBoot_(networkBoot), start_(start) {}

BorgesActivity::~BorgesActivity() = default;

void BorgesActivity::onEnter() {
  Activity::onEnter();
  stack_ = makeStack();
  stackOk_ = stack_ && stack_->openStore() && stack_->openEngine();
  if (start_ == NetPayload::ManualFromReader) ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  buildRows();
  fullPaint_ = true;
  if (networkBoot_ && stackOk_) {
    if (start_ == NetPayload::Pair) {
      startPair();
      return;
    }
    if (start_ == NetPayload::ManualFromHome || start_ == NetPayload::ManualFromReader) {
      startSync();
      return;
    }
  }
  requestUpdate();
}

void BorgesActivity::onExit() {
  if (stack_) stack_->cancel.request();
  if (!worker_.waitForExit(4000)) {
    stack_.release();  // Only on the way to deep sleep, which ends every task.
    pairing_.release();
  } else if (stack_) {
    stack_->bringDown();
  }
  Activity::onExit();
}

void BorgesActivity::buildRows() {
  rows_.clear();
  rows_.reserve(6);
  const bool paired = stackOk_ && stack_->store.creds().paired();
  const bool relink = paired && stack_->store.state().needsRelink;
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
  if (!stack_->glueStore.save(stack_->glue)) LOG_ERR("BORGES", "Could not save preferences");
}

void BorgesActivity::setView(const View view) {
  view_ = view;
  fullPaint_ = true;
  requestUpdate();
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
        stack_->bringDown();
        stack_.reset();
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
      stack_->glue.autoOnOpen = !stack_->glue.autoOnOpen;
      savePrefs();
      requestUpdate();
      return;
    case Row::OnSleep:
      stack_->glue.syncOnSleep = !stack_->glue.syncOnSleep;
      savePrefs();
      requestUpdate();
      return;
    case Row::Disconnect:
      startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_BORGES_DISCONNECT),
                                                                    tr(STR_BORGES_DISCONNECT_CONFIRM)),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 // The credential goes, and only it: what is queued waits for a new pairing.
                                 if (!stack_->store.forgetCredential())
                                   LOG_ERR("BORGES", "Could not forget the credential");
                                 focus_ = 0;
                                 buildRows();
                               }
                               fullPaint_ = true;
                             });
      return;
  }
}

// ---------------------------------------------------------------------------------
// Manual sync
// ---------------------------------------------------------------------------------

void BorgesActivity::startSync() {
  if (!hasSavedWifi() && !stack_->link.connected()) {
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
  stack_->cancel.arm(kManualBudgetMs);
  if (start_ == NetPayload::ManualFromReader && !APP_STATE.openEpubPath.empty()) {
    bookPath_ = APP_STATE.openEpubPath;
    bookTitle_ = bookTitleFor(bookPath_);
    bookRef_ = makeUniqueNoThrow<BookRef>();
    local_ = makeUniqueNoThrow<Position>();
    decision_ = makeUniqueNoThrow<ResumeDecision>();
  }
  setView(View::Syncing);
  if (!worker_.start(&BorgesActivity::syncMain, this)) {
    resultText_.store(StrId::STR_BORGES_ERR_LOCAL);
    resultOk_.store(false);
    setView(View::SyncResult);
    autoLeaveAt_ = millis() + kResultErrorMs;
  }
}

void BorgesActivity::syncMain(void* self) {
  auto* activity = static_cast<BorgesActivity*>(self);
  BorgesStack& stack = *activity->stack_;
  const auto finish = [activity](const StrId text, const bool ok) {
    activity->resultText_.store(text);
    activity->resultOk_.store(ok);
    activity->requestedView_.store(View::SyncResult);
    activity->viewChanged_.store(true, std::memory_order_release);
  };

  const net::LinkResult link = stack.bringUp(kSyncLinkBudgetMs);
  if (link != net::LinkResult::Connected) {
    finish(describeLink(link), false);
    return;
  }

  const bool withBook = activity->bookRef_ && activity->local_ && activity->decision_ &&
                        partialMd5(stack.fs, activity->bookPath_.c_str(), activity->bookRef_->id);
  if (withBook) {
    activity->bookRef_->title = activity->bookTitle_.c_str();
    const float percent = localPercentFor(activity->bookPath_);
    if (percent >= 0) activity->local_->setPercent(percent);
  }
  const SessionReport report =
      withBook
          ? stack.engine.runManualSession(activity->bookRef_.get(), activity->local_.get(), activity->decision_.get())
          : stack.engine.runManualSession(nullptr, nullptr, nullptr);
  if (report.outcome == SessionOutcome::AwaitingChoice) {
    activity->needsChoice_.store(true);
    activity->viewChanged_.store(true, std::memory_order_release);
    return;
  }
  stack.bringDown();
  finish(describeOutcome(report.outcome), report.outcome == SessionOutcome::Ok);
}

// ---------------------------------------------------------------------------------
// Pairing
// ---------------------------------------------------------------------------------

void BorgesActivity::startPair() {
  if (!hasSavedWifi() && !stack_->link.connected()) {
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
  config.externalId = stack_->externalId;
  // The account lists devices by this name. "CrossInk X4" says less than "X4".
  bookTitle_ = SETTINGS.getEffectiveDeviceName();
  constexpr char kPrefix[] = "CrossInk ";
  if (bookTitle_.rfind(kPrefix, 0) == 0) bookTitle_.erase(0, sizeof(kPrefix) - 1);
  config.deviceName = bookTitle_.c_str();
  config.firmwareVersion = stack_->clientVersion + strlen("crossink-borges/");
  config.clientVersion = stack_->clientVersion;
  pairing_ = makeUniqueNoThrow<PairingFlow>(stack_->store, stack_->outbox, stack_->fs, stack_->http, stack_->clock,
                                            stack_->random, stack_->cancel, config);
  if (!pairing_) {
    resultText_.store(StrId::STR_BORGES_ERR_LOCAL);
    resultOk_.store(false);
    setView(View::PairResult);
    return;
  }
  stack_->cancel.arm(0);
  setView(View::PairConnecting);
  if (!worker_.start(&BorgesActivity::pairMain, this)) {
    resultText_.store(StrId::STR_BORGES_ERR_LOCAL);
    resultOk_.store(false);
    setView(View::PairResult);
  }
}

void BorgesActivity::pairMain(void* self) {
  auto* activity = static_cast<BorgesActivity*>(self);
  BorgesStack& stack = *activity->stack_;
  PairingFlow& flow = *activity->pairing_;
  const auto show = [activity](const View view) {
    activity->requestedView_.store(view);
    activity->viewChanged_.store(true, std::memory_order_release);
  };
  const auto finish = [&](const StrId text, const bool ok) {
    stack.bringDown();
    activity->resultText_.store(text);
    activity->resultOk_.store(ok);
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
  if (stack_) stack_->cancel.request();
  (void)worker_.waitForExit(4000);
  if (networkBoot_) {
    if (start_ == NetPayload::ManualFromReader) {
      silentRestartToReader(true);
    } else {
      silentRestart();
    }
    return;
  }
  finish();
}

void BorgesActivity::loop() {
  if (viewChanged_.exchange(false, std::memory_order_acquire)) {
    if (needsChoice_.exchange(false)) {
      auto resume = makeUniqueNoThrow<BorgesResumeActivity>(
          renderer, mappedInput, std::move(stack_), std::move(decision_), bookPath_, bookTitle_,
          start_ == NetPayload::ManualFromReader ? BorgesResumeActivity::Return::Reader
                                                 : BorgesResumeActivity::Return::Status);
      if (resume) {
        activityManager.replaceActivity(std::move(resume));
      } else {
        leave();
      }
      return;
    }
    const View next = requestedView_.load();
    if (next == View::SyncResult || next == View::PairResult) {
      autoLeaveAt_ = millis() + (resultOk_.load() ? kResultOkMs : kResultErrorMs);
      const Diagnostics& diagnostics = stack_->store.state().diag;
      snprintf(requestId_, sizeof(requestId_), "%s", resultOk_.load() ? "" : diagnostics.requestId);
    }
    setView(next);
    return;
  }

  switch (view_) {
    case View::Syncing:
    case View::PairConnecting:
    case View::PairCode:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        stack_->cancel.request();
        if (!worker_.waitForExit(4000)) {
          leave();
          return;
        }
        stack_->bringDown();
        pairing_.reset();
        if (start_ == NetPayload::ManualFromReader) {
          leave();
        } else {
          buildRows();
          setView(View::Status);
        }
      }
      return;
    case View::SyncResult:
    case View::PairResult:
      if (mappedInput.wasAnyPressed() || millis() >= autoLeaveAt_) {
        if (start_ == NetPayload::ManualFromReader) {
          leave();
          return;
        }
        pairing_.reset();
        buildRows();
        setView(View::Status);
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

void BorgesActivity::renderPairCode() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int headerBottom = metrics.topPadding + metrics.headerHeight;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_BORGES_CONNECT));

  Rect area = contentArea(renderer, headerBottom + kBlockGap);
  area.x += kSideMargin;
  area.width -= kSideMargin * 2;

  // Step one is the phone. The URL is what a person types when a QR will not scan.
  std::string url = pairing_->qrUrl();
  const size_t scheme = url.find("://");
  if (scheme != std::string::npos) url.erase(0, scheme + 3);
  const size_t query = url.find('?');
  if (query != std::string::npos) url.resize(query);
  char line[160];
  snprintf(line, sizeof(line), tr(STR_BORGES_PAIR_OPEN_FMT), url.c_str());
  int y = drawWrappedCentered(renderer, area, UI_10_FONT_ID, area.y, line, 3);
  y += kBlockGap;

#if CROSSINK_SCALABLE_FONTS
  constexpr int kCodeFont = UI_12_FONT_ID;
#else
  constexpr int kCodeFont = LEXENDDECA_16_FONT_ID;
#endif
  // Wide-set: read aloud or typed, a character at a time.
  const char* code = pairing_->userCode();
  constexpr int kCodeGap = 6;
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
  y += renderer.getLineHeight(kCodeFont) + kBlockGap;

  const int footer = renderer.getLineHeight(SMALL_FONT_ID) + kBlockGap;
  const int available = area.y + area.height - y - footer;
  const int qr = std::min(area.width, available);
  if (qr > 96) {
    const Rect bounds{area.x + (area.width - qr) / 2, y, qr, qr};
    QrUtils::drawQrCode(renderer, bounds, pairing_->qrUrl());
  }
  drawWrappedCentered(renderer, area, SMALL_FONT_ID, area.y + area.height - footer, tr(STR_BORGES_PAIR_WAITING), 1);
}

void BorgesActivity::renderStatus() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_BORGES));
  int y = metrics.topPadding + metrics.headerHeight + kBlockGap;
  const int x = kSideMargin;
  const int width = renderer.getScreenWidth() - kSideMargin * 2;

  const bool paired = stackOk_ && stack_->store.creds().paired();
  if (!paired) {
    y = drawWrapped(renderer, UI_12_FONT_ID, x, y, width, tr(STR_BORGES_NOT_CONNECTED), 2, EpdFontFamily::BOLD);
    y += kLineGap;
    y = drawWrapped(renderer, UI_10_FONT_ID, x, y, width, tr(STR_BORGES_NOT_CONNECTED_HINT), 4);
    y += kBlockGap;
  } else {
    const EngineStatus status = stack_->engine.status();
    const Credentials& creds = stack_->store.creds();
    char value[48];

    snprintf(value, sizeof(value), "%.8s", creds.accountId[0] ? creds.accountId : creds.deviceId);
    y = drawLabelValue(renderer, x, y, width, tr(STR_BORGES_ACCOUNT), value) + kLineGap;

    if (status.lastSuccessEpoch <= 0) {
      snprintf(value, sizeof(value), "%s", tr(STR_BORGES_NEVER));
    } else {
      formatAge(value, sizeof(value), ageOf(status.lastSuccessEpoch, stack_->clock.wallNow()));
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
      if (status.diag.requestId[0]) {
        snprintf(value, sizeof(value), tr(STR_BORGES_REQUEST_ID_FMT), status.diag.requestId);
        y = drawWrapped(renderer, SMALL_FONT_ID, x, y, width, value, 2);
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
        return tr(STR_BORGES_LIBRARY);
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
    if (rows_[index] == Row::OnOpen) return stack_->glue.autoOnOpen ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    if (rows_[index] == Row::OnSleep) return stack_->glue.syncOnSleep ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    return {};
  };
  GUI.drawList(renderer, list, static_cast<int>(rows_.size()), focus_, labelOf, nullptr, nullptr, valueOf);

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), rows_.empty() ? "" : tr(STR_SELECT),
                            rows_.size() > 1 ? tr(STR_DIR_UP) : "", rows_.size() > 1 ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BorgesActivity::render(RenderLock&&) {
  renderer.clearScreen();
  switch (view_) {
    case View::Status:
      renderStatus();
      break;
    case View::Syncing:
      renderCentered(tr(STR_BORGES_SYNCING), nullptr);
      break;
    case View::PairConnecting:
      renderCentered(tr(STR_BORGES_PAIR_CONNECTING), nullptr);
      break;
    case View::PairCode:
      renderPairCode();
      break;
    case View::SyncResult:
    case View::PairResult: {
      char id[64] = {0};
      if (requestId_[0]) snprintf(id, sizeof(id), tr(STR_BORGES_REQUEST_ID_FMT), requestId_);
      renderCentered(I18N.get(resultText_.load()), id);
      break;
    }
  }
  if (view_ == View::Syncing || view_ == View::PairCode || view_ == View::PairConnecting) {
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer(fullPaint_ ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  fullPaint_ = false;
}

}  // namespace glue
}  // namespace borges
