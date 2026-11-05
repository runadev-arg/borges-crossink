#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "BorgesActivities.h"
#include "BorgesUi.h"
#include "CrossPointState.h"
#include "LibraryBuilder.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace borges {
namespace glue {

namespace {
constexpr const char* kBorgesBooksDir = "/Borges";
constexpr uint32_t kLoadLinkBudgetMs = 15000;
constexpr uint32_t kFetchBudgetMs = 30000;
constexpr uint32_t kDownloadBudgetMs = 15UL * 60UL * 1000UL;
constexpr unsigned long kProgressPaintMs = 2500;
constexpr uint32_t kProgressPaintStep = 5;
// Reading a list needs no radio. The next page or download raises it again.
constexpr unsigned long kIdleLinkMs = 30000;
// A session past what it can see through ICancel is hung. Every way out of here is a reboot.
constexpr unsigned long kHangMarginMs = 15000;
constexpr unsigned long kDownloadStallMs = 120000;  // No byte for this long: past any socket timeout.
constexpr unsigned long kCancelWaitMs = 5000;
constexpr uint32_t kLeaveWaitMs = 1500;

int rowHeightFor(const GfxRenderer& renderer) {
  return renderer.getLineHeight(UI_10_FONT_ID) + renderer.getLineHeight(SMALL_FONT_ID) + 14;
}

// The foot of the list belongs to the note ("offline, showing the last list"). A full page used to
// cover it, so the one thing that explained a stale list was the one thing not shown.
int noteHeightFor(const GfxRenderer& renderer) { return renderer.getLineHeight(SMALL_FONT_ID) + kLineGap; }

Rect listArea(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  Rect area = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int top = metrics.topPadding + metrics.headerHeight + kLineGap;
  const int bottom = area.y + area.height;
  area.y = top;
  area.height = bottom - top;
  return area;
}
}  // namespace

class BorgesLibraryActivity::Progress final : public IDownloadProgress {
 public:
  explicit Progress(std::atomic<uint32_t>& percent) : percent_(percent) {}
  void onProgress(const uint64_t received, const uint64_t total) override {
    percent_.store(total ? static_cast<uint32_t>(received * 100 / total) : 0, std::memory_order_relaxed);
  }

 private:
  std::atomic<uint32_t>& percent_;
};

struct BorgesLibraryActivity::Shared {
  std::unique_ptr<BorgesStack> stack_;
  std::unique_ptr<Catalog> catalog_;
  std::unique_ptr<Progress> progress_;
  std::vector<CatalogEntry> entries_;
  std::vector<uint8_t> onSd_;
  uint16_t page_ = 1;
  uint16_t requestedPage_ = 1;
  bool hasNext_ = false;
  bool hasPrevious_ = false;
  CatalogEntry downloading_;
  std::atomic<View> requestedView_{View::Loading};
  std::atomic<bool> viewChanged_{false};
  std::atomic<StrId> note_{StrId::STR_NONE_OPT};
  std::atomic<bool> hasNote_{false};
  std::atomic<uint32_t> downloadPercent_{0};
  std::atomic<bool> linked_{false};  // The radio is up: what the screen says moves on from "connecting".
};

struct BorgesLibraryActivity::Visitor final : ICatalogVisitor {
  explicit Visitor(Shared& shared) : shared(shared) {}
  void onBook(uint16_t, const CatalogEntry& entry) override {
    if (shared.entries_.size() < shared.entries_.capacity()) {
      shared.entries_.push_back(entry);
      shared.onSd_.push_back(shared.catalog_->isOnSd(entry, kBorgesBooksDir) ? 1 : 0);
    }
  }
  Shared& shared;
};

BorgesLibraryActivity::BorgesLibraryActivity(GfxRenderer& renderer, MappedInputManager& input)
    : Activity("BorgesLibrary", renderer, input) {}

BorgesLibraryActivity::~BorgesLibraryActivity() = default;

int BorgesLibraryActivity::pageSize() const {
  const int rows = (listArea(renderer).height - noteHeightFor(renderer)) / rowHeightFor(renderer);
  return std::max(1, std::min(rows, static_cast<int>(Catalog::kMaxPageSize)));
}

void BorgesLibraryActivity::onEnter() {
  Activity::onEnter();
  s_ = makeUniqueNoThrow<Shared>();
  if (!s_) {
    silentRestart();
    return;
  }
  s_->stack_ = makeStack();
  if (!s_->stack_ || !s_->stack_->openStore()) {
    silentRestart();
    return;
  }
  s_->catalog_ = makeUniqueNoThrow<Catalog>(s_->stack_->store, s_->stack_->fs, s_->stack_->http, s_->stack_->clock,
                                            s_->stack_->cancel, s_->stack_->sha);
  s_->progress_ = makeUniqueNoThrow<Progress>(s_->downloadPercent_);
  if (!s_->catalog_ || !s_->progress_) {
    silentRestart();
    return;
  }
  // One screen of entries: under 400 bytes each, kept for this activity's lifetime.
  s_->entries_.reserve(pageSize());
  s_->onSd_.reserve(pageSize());
  if (!hasSavedWifi() && !s_->stack_->link.connected()) {
    // First network ever: the picker owns the connection and the password, as it does for a sync.
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, true, true),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               leave();
                             } else {
                               startLoad(1);
                             }
                           });
    return;
  }
  startLoad(1);
}

void BorgesLibraryActivity::onExit() {
  if (s_ && s_->stack_) s_->stack_->cancel.request();
  if (!worker_.waitForExit(4000)) {
    // Only on the way to deep sleep, which ends every task.
    (void)s_.release();
  } else if (s_ && s_->stack_) {
    s_->stack_->bringDown();
  }
  Activity::onExit();
}

void BorgesLibraryActivity::setView(const View view) {
  const View previous = view_;
  view_ = view;
  // A slow refresh clears a dense list and is the one a new list needs. The first list over a
  // sparse screen, and text over text, repaint fast: a page change is one slow refresh, not two.
  // A pending first paint (over what the boot left) stays pending.
  if ((view == View::List && listPainted_) || (previous == View::List && view != View::Loading)) fullPaint_ = true;
  linkDownAt_ = millis() + kIdleLinkMs;
  requestUpdate();
}

// The wait for a session to stop is a second or two: say so first.
void BorgesLibraryActivity::cancelAndLeave() {
  cancelling_ = true;
  (void)requestUpdateAndWait();
  leave();
}

// What the radio is doing, for the two screens that wait on it.
void BorgesLibraryActivity::noteLink(const bool up) {
  linkShown_ = up;
  s_->linked_.store(up);
}

void BorgesLibraryActivity::startLoad(const uint16_t page) {
  s_->requestedPage_ = page;
  s_->hasNote_.store(false);
  noteLink(s_->stack_->link.connected());
  s_->stack_->cancel.arm(kLoadLinkBudgetMs + kFetchBudgetMs);
  hardDeadlineAt_ = millis() + kLoadLinkBudgetMs + kFetchBudgetMs + kHangMarginMs;
  setView(View::Loading);
  if (!worker_.start(&BorgesLibraryActivity::loadMain, s_.get())) silentRestart();
}

void BorgesLibraryActivity::loadMain(void* shared) {
  auto* s = static_cast<BorgesLibraryActivity::Shared*>(shared);
  BorgesStack& stack = *s->stack_;
  const auto note = [s](const StrId text) {
    s->note_.store(text);
    s->hasNote_.store(true);
  };

  CatalogPage info;
  CatalogResult result = CatalogResult::Offline;
  const net::LinkResult link = stack.bringUp(kLoadLinkBudgetMs);
  if (link == net::LinkResult::Connected) {
    s->linked_.store(true);
    result = s->catalog_->fetchPage(s->requestedPage_, s->entries_.capacity(), info, kFetchBudgetMs);
  }

  s->entries_.clear();
  s->onSd_.clear();
  Visitor visitor(*s);
  const bool loaded = s->catalog_->loadPage(s->requestedPage_, info, visitor);
  if (loaded) {
    s->page_ = s->requestedPage_;
    s->hasNext_ = info.hasNext;
    s->hasPrevious_ = info.hasPrevious;
  }
  switch (result) {
    case CatalogResult::Ok:
      break;
    case CatalogResult::NeedsRelink:
      note(StrId::STR_BORGES_ERR_RELINK);
      break;
    case CatalogResult::Offline:
    case CatalogResult::Cancelled:
      note(loaded ? StrId::STR_BORGES_LIB_OFFLINE : describeLink(link));
      break;
    case CatalogResult::ServerError:
    case CatalogResult::Malformed:
      note(loaded ? StrId::STR_BORGES_LIB_OFFLINE : StrId::STR_BORGES_ERR_SERVER);
      break;
    case CatalogResult::LocalError:
      note(StrId::STR_BORGES_ERR_LOCAL);
      break;
  }
  s->requestedView_.store(View::List);
  s->viewChanged_.store(true, std::memory_order_release);
}

void BorgesLibraryActivity::startDownload() {
  s_->downloading_ = s_->entries_[focus_];
  s_->downloadPercent_.store(0);
  shownPercent_ = 0;
  lastProgressPaint_ = millis();
  s_->stack_->cancel.arm(kDownloadBudgetMs);
  hardDeadlineAt_ = 0;
  lastProgressAt_ = millis();
  lastProgressPercent_ = 0;
  noteLink(s_->stack_->link.connected());
  setView(View::Downloading);
  if (!worker_.start(&BorgesLibraryActivity::downloadMain, s_.get())) {
    s_->note_.store(StrId::STR_BORGES_ERR_LOCAL);
    setView(View::DownloadFailed);
  }
}

void BorgesLibraryActivity::downloadMain(void* shared) {
  auto* s = static_cast<BorgesLibraryActivity::Shared*>(shared);
  BorgesStack& stack = *s->stack_;
  const auto done = [s](const View view) {
    s->requestedView_.store(view);
    s->viewChanged_.store(true, std::memory_order_release);
  };
  const auto fail = [s, &done](const StrId text) {
    s->note_.store(text);
    done(View::DownloadFailed);
  };

  if (!stack.fs.mkdir(kBorgesBooksDir)) {
    fail(StrId::STR_BORGES_ERR_LOCAL);
    return;
  }
  const net::LinkResult link = stack.bringUp(kLoadLinkBudgetMs);
  if (link != net::LinkResult::Connected) {
    fail(describeLink(link));
    return;
  }
  s->linked_.store(true);
  switch (s->catalog_->download(s->downloading_, kBorgesBooksDir, s->progress_.get(), kDownloadBudgetMs)) {
    case DownloadResult::Done:
    case DownloadResult::AlreadyPresent:
      library::invalidateLibraryIndex();
      for (size_t i = 0; i < s->entries_.size(); ++i) {
        if (s->catalog_->isOnSd(s->entries_[i], kBorgesBooksDir)) s->onSd_[i] = 1;
      }
      done(View::Downloaded);
      return;
    case DownloadResult::Interrupted:
      fail(StrId::STR_BORGES_LIB_INTERRUPTED);
      return;
    case DownloadResult::Cancelled:
      s->requestedView_.store(View::List);
      s->viewChanged_.store(true, std::memory_order_release);
      return;
    case DownloadResult::NeedsRelink:
      fail(StrId::STR_BORGES_ERR_RELINK);
      return;
    case DownloadResult::ServerError:
      fail(StrId::STR_BORGES_ERR_SERVER);
      return;
    case DownloadResult::IntegrityFailed:
      fail(StrId::STR_BORGES_LIB_INTEGRITY);
      return;
    case DownloadResult::LocalError:
      fail(StrId::STR_BORGES_ERR_LOCAL);
      return;
  }
}

void BorgesLibraryActivity::openDownloaded() {
  char path[128];
  snprintf(path, sizeof(path), "%s/%s", kBorgesBooksDir, s_->downloading_.fileName);
  s_->stack_->bringDown();
  s_->catalog_.reset();
  s_->stack_.reset();  // interceptOpen builds its own, and this boot's heap is already tight.
  APP_STATE.openEpubPath = path;
  APP_STATE.saveToFile();
  // The same door as any other open: the account may have read this book elsewhere, and a
  // fresh download has no local place to protect it from opening at page 1.
  if (auto check = interceptOpen(renderer, mappedInput, path)) {
    activityManager.replaceActivity(std::move(check));
    return;
  }
  silentRestartToReader(true);
}

void BorgesLibraryActivity::leave() {
  if (s_->stack_) s_->stack_->cancel.request();
  if (!worker_.waitForExit(kLeaveWaitMs)) {
    silentRestart();
    return;
  }
  s_->stack_->bringDown();
  s_->catalog_.reset();
  s_->stack_.reset();
  auto status = makeUniqueNoThrow<BorgesActivity>(renderer, mappedInput, true, NetPayload::Status);
  if (status) {
    activityManager.replaceActivity(std::move(status));
  } else {
    silentRestart();
  }
}

void BorgesLibraryActivity::loop() {
  if (s_->viewChanged_.exchange(false, std::memory_order_acquire)) {
    const View next = s_->requestedView_.load();
    if (next == View::List && (focus_ >= static_cast<int>(s_->entries_.size()) || wantLastRow_)) {
      focus_ = std::max(0, static_cast<int>(s_->entries_.size()) - 1);
    }
    wantLastRow_ = false;
    cancelling_ = false;
    worker_.reset();
    setView(next);
    return;
  }

  if ((view_ == View::Loading || view_ == View::Downloading) && !linkShown_ && s_->linked_.load()) {
    linkShown_ = true;  // The radio came up: the screen moves on to what it is doing now.
    requestUpdate();
  }

  if (linkDownAt_ != 0 && millis() >= linkDownAt_ && !worker_.running()) {
    s_->stack_->bringDown();
    linkDownAt_ = 0;
  }

  switch (view_) {
    case View::Loading:
      if (hardDeadlineAt_ != 0 && millis() >= hardDeadlineAt_) {
        LOG_ERR("BORGES", "Library load hung: leaving without it");
        silentRestart();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelAndLeave();
      return;
    case View::Downloading: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        s_->stack_->cancel.request();
        hardDeadlineAt_ = millis() + kCancelWaitMs;
        cancelling_ = true;
        requestUpdate();
        return;
      }
      const uint32_t received = s_->downloadPercent_.load(std::memory_order_relaxed);
      if (received != lastProgressPercent_) {
        lastProgressPercent_ = received;
        lastProgressAt_ = millis();
      }
      if ((hardDeadlineAt_ != 0 && millis() >= hardDeadlineAt_) || millis() - lastProgressAt_ >= kDownloadStallMs) {
        // The .part stays: the next try picks up where this one stopped.
        LOG_ERR("BORGES", "Download hung: leaving without it");
        silentRestart();
        return;
      }
      // E-ink: a bar that moves every few seconds, never one that flickers.
      const uint32_t percent = s_->downloadPercent_.load(std::memory_order_relaxed);
      if (percent >= shownPercent_ + kProgressPaintStep && millis() - lastProgressPaint_ >= kProgressPaintMs) {
        shownPercent_ = percent;
        lastProgressPaint_ = millis();
        requestUpdate();
      }
      return;
    }
    case View::Downloaded: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        setView(View::List);
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        if (downloadFocus_ == 0) {
          openDownloaded();
        } else {
          setView(View::List);
        }
        return;
      }
      const auto move = [this] {
        downloadFocus_ = downloadFocus_ == 0 ? 1 : 0;
        requestUpdate();
      };
      navigator_.onNextRelease(move);
      navigator_.onPreviousRelease(move);
      return;
    }
    case View::DownloadFailed:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        setView(View::List);
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        if (s_->note_.load() == StrId::STR_BORGES_ERR_RELINK) {
          setView(View::List);  // Trying again cannot help: the reader has to be linked again.
        } else {
          startDownload();  // Picks up the .part where it stopped.
        }
      }
      return;
    case View::List:
      break;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    leave();
    return;
  }
  const int count = static_cast<int>(s_->entries_.size());
  if (count == 0) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) startLoad(s_->page_);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (s_->onSd_[focus_]) {
      // Already here: the button says "Open", so it opens.
      s_->downloading_ = s_->entries_[focus_];
      openDownloaded();
    } else {
      startDownload();
    }
    return;
  }
  navigator_.onNextRelease([this, count] {
    if (focus_ + 1 >= count && s_->hasNext_) {
      focus_ = 0;
      startLoad(s_->page_ + 1);
      return;
    }
    focus_ = ButtonNavigator::nextIndex(focus_, count);
    requestUpdate();
  });
  navigator_.onPreviousRelease([this, count] {
    if (focus_ == 0 && s_->hasPrevious_) {
      wantLastRow_ = true;
      startLoad(s_->page_ - 1);
      return;
    }
    focus_ = ButtonNavigator::previousIndex(focus_, count);
    requestUpdate();
  });
}

void BorgesLibraryActivity::renderList() {
  const Rect area = listArea(renderer);
  const int rowHeight = rowHeightFor(renderer);
  const int padding = kSideMargin;
  constexpr int kCheckSize = 16;
  int y = area.y;

  if (s_->entries_.empty()) {
    Rect text = area;
    text.x += padding;
    text.width -= padding * 2;
    const StrId message = s_->hasNote_.load() ? s_->note_.load() : StrId::STR_BORGES_LIB_EMPTY;
    drawWrappedCentered(renderer, text, UI_10_FONT_ID, area.y + area.height / 3, I18N.get(message), 3);
    return;
  }

  char size[16];
  for (size_t i = 0; i < s_->entries_.size(); ++i) {
    const CatalogEntry& entry = s_->entries_[i];
    const bool focused = static_cast<int>(i) == focus_;
    if (focused) renderer.fillRect(area.x, y, area.width, rowHeight, true);
    const int textWidth = area.width - padding * 2 - (s_->onSd_[i] ? kCheckSize + 12 : 0);
    const std::string title = renderer.truncatedText(UI_10_FONT_ID, entry.title, textWidth);
    renderer.drawText(UI_10_FONT_ID, area.x + padding, y + 6, title.c_str(), !focused);

    formatSize(size, sizeof(size), entry.fileSize);
    std::string meta = entry.author;
    if (!meta.empty()) meta += " \xC2\xB7 ";
    meta += size;
    const std::string metaLine = renderer.truncatedText(SMALL_FONT_ID, meta.c_str(), textWidth);
    renderer.drawText(SMALL_FONT_ID, area.x + padding, y + 6 + renderer.getLineHeight(UI_10_FONT_ID), metaLine.c_str(),
                      !focused);
    if (s_->onSd_[i]) {
      drawCheckMark(renderer, area.x + area.width - padding - kCheckSize, y + (rowHeight - kCheckSize) / 2, kCheckSize,
                    !focused);
    }
    y += rowHeight;
  }
  if (s_->hasNote_.load()) {
    const int noteY = area.y + area.height - noteHeightFor(renderer) + kLineGap;
    const std::string note = renderer.truncatedText(SMALL_FONT_ID, I18N.get(s_->note_.load()), area.width - padding * 2);
    renderer.drawText(SMALL_FONT_ID, area.x + padding, noteY, note.c_str());
  }
}

void BorgesLibraryActivity::renderDownload() {
  Rect area = listArea(renderer);
  area.x += kSideMargin;
  area.width -= kSideMargin * 2;
  int y = area.y + kBlockGap * 2;
  y = drawWrapped(renderer, UI_12_FONT_ID, area.x, y, area.width, s_->downloading_.title, 3, EpdFontFamily::BOLD);
  if (s_->downloading_.author[0]) {
    y = drawWrapped(renderer, UI_10_FONT_ID, area.x, y + kLineGap, area.width, s_->downloading_.author, 1);
  }
  y += kBlockGap * 2;

  if (view_ == View::Downloading) {
    const char* what = cancelling_ ? tr(STR_BORGES_CANCELLING)
                       : linkShown_ ? tr(STR_BORGES_LIB_DOWNLOADING)
                                    : tr(STR_BORGES_CONNECTING_WIFI);
    y = drawWrapped(renderer, UI_10_FONT_ID, area.x, y, area.width, what, 1);
    y += kLineGap;
    const uint32_t percent = shownPercent_;
    drawProgressBar(renderer, area.x, y, area.width, 12, percent);
    y += 12 + kLineGap;
    char text[48];
    char total[16];
    formatSize(total, sizeof(total), s_->downloading_.fileSize);
    snprintf(text, sizeof(text), "%u%% \xC2\xB7 %s", static_cast<unsigned>(percent), total);
    renderer.drawText(SMALL_FONT_ID, area.x, y, text);
  } else if (view_ == View::Downloaded) {
    y = drawWrapped(renderer, UI_10_FONT_ID, area.x, y, area.width, tr(STR_BORGES_LIB_DOWNLOADED), 1,
                    EpdFontFamily::BOLD);
    y += kBlockGap;
    const int rowHeight = actionRowHeight(renderer);
    drawActionRow(renderer, area.x, y, area.width, rowHeight, tr(STR_BORGES_LIB_OPEN_NOW), downloadFocus_ == 0);
    drawActionRow(renderer, area.x, y + rowHeight + 8, area.width, rowHeight, tr(STR_BORGES_LIB_KEEP_BROWSING),
                  downloadFocus_ == 1);
  } else {
    // The button's own label says what it does now: there is no sentence to explain it.
    drawWrapped(renderer, UI_10_FONT_ID, area.x, y, area.width, I18N.get(s_->note_.load()), 4);
  }
}

void BorgesLibraryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  char subtitle[24] = {0};
  if (view_ == View::List && (s_->hasNext_ || s_->hasPrevious_)) {
    snprintf(subtitle, sizeof(subtitle), tr(STR_BORGES_LIB_PAGE_FMT), static_cast<unsigned>(s_->page_));
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_BORGES_LIBRARY), subtitle[0] ? subtitle : nullptr);

  const char* confirm = "";
  const char* up = "";
  const char* down = "";
  const char* backLabel = tr(STR_BACK);
  switch (view_) {
    case View::Loading: {
      Rect area = listArea(renderer);
      area.x += kSideMargin;
      area.width -= kSideMargin * 2;
      const char* what = cancelling_ ? tr(STR_BORGES_CANCELLING)
                         : linkShown_ ? tr(STR_BORGES_LIB_LOADING)
                                      : tr(STR_BORGES_CONNECTING_WIFI);
      drawWrappedCentered(renderer, area, UI_10_FONT_ID, area.y + area.height / 3, what, 2);
      break;
    }
    case View::List:
      renderList();
      listPainted_ = true;
      if (s_->entries_.empty()) {
        confirm = tr(STR_BORGES_TRY);
      } else {
        // The button names what it does to the book under the focus.
        confirm = s_->onSd_[focus_] ? tr(STR_OPEN) : tr(STR_BORGES_GET);
        up = tr(STR_DIR_UP);
        down = tr(STR_DIR_DOWN);
      }
      break;
    case View::Downloading:
      renderDownload();
      backLabel = tr(STR_CANCEL);
      break;
    case View::Downloaded:
      renderDownload();
      confirm = tr(STR_SELECT);
      up = tr(STR_DIR_UP);
      down = tr(STR_DIR_DOWN);
      break;
    case View::DownloadFailed:
      renderDownload();
      confirm = s_->note_.load() == StrId::STR_BORGES_ERR_RELINK ? tr(STR_BORGES_CONTINUE) : tr(STR_BORGES_TRY);
      break;
  }
  const auto labels = mappedInput.mapLabels(
      view_ == View::Downloading ? MappedInputManager::Label(backLabel) : mappedInput.withBackArrow(backLabel), confirm,
      up, down);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(fullPaint_ ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  fullPaint_ = false;
}

}  // namespace glue
}  // namespace borges
