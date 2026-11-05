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

int rowHeightFor(const GfxRenderer& renderer) {
  return renderer.getLineHeight(UI_10_FONT_ID) + renderer.getLineHeight(SMALL_FONT_ID) + 14;
}

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

struct BorgesLibraryActivity::Visitor final : ICatalogVisitor {
  explicit Visitor(BorgesLibraryActivity& activity) : activity(activity) {}
  void onBook(uint16_t, const CatalogEntry& entry) override {
    if (activity.entries_.size() < activity.entries_.capacity()) {
      activity.entries_.push_back(entry);
      activity.onSd_.push_back(activity.catalog_->isOnSd(entry, kBorgesBooksDir) ? 1 : 0);
    }
  }
  BorgesLibraryActivity& activity;
};

BorgesLibraryActivity::BorgesLibraryActivity(GfxRenderer& renderer, MappedInputManager& input)
    : Activity("BorgesLibrary", renderer, input) {}

BorgesLibraryActivity::~BorgesLibraryActivity() = default;

int BorgesLibraryActivity::pageSize() const {
  const int rows = listArea(renderer).height / rowHeightFor(renderer);
  return std::max(1, std::min(rows, static_cast<int>(Catalog::kMaxPageSize)));
}

void BorgesLibraryActivity::onEnter() {
  Activity::onEnter();
  stack_ = makeStack();
  if (!stack_ || !stack_->openStore()) {
    silentRestart();
    return;
  }
  catalog_ = makeUniqueNoThrow<Catalog>(stack_->store, stack_->fs, stack_->http, stack_->clock, stack_->cancel);
  progress_ = makeUniqueNoThrow<Progress>(downloadPercent_);
  if (!catalog_ || !progress_) {
    silentRestart();
    return;
  }
  // One screen of entries: under 400 bytes each, kept for this activity's lifetime.
  entries_.reserve(pageSize());
  onSd_.reserve(pageSize());
  startLoad(1);
}

void BorgesLibraryActivity::onExit() {
  if (stack_) stack_->cancel.request();
  if (!worker_.waitForExit(4000)) {
    stack_.release();  // Only on the way to deep sleep, which ends every task.
    catalog_.release();
    progress_.release();
  } else if (stack_) {
    stack_->bringDown();
  }
  Activity::onExit();
}

void BorgesLibraryActivity::setView(const View view) {
  view_ = view;
  fullPaint_ = true;
  requestUpdate();
}

void BorgesLibraryActivity::startLoad(const uint16_t page) {
  requestedPage_ = page;
  hasNote_.store(false);
  stack_->cancel.arm(kLoadLinkBudgetMs + kFetchBudgetMs);
  setView(View::Loading);
  if (!worker_.start(&BorgesLibraryActivity::loadMain, this)) silentRestart();
}

void BorgesLibraryActivity::loadMain(void* self) {
  auto* activity = static_cast<BorgesLibraryActivity*>(self);
  BorgesStack& stack = *activity->stack_;
  const auto note = [activity](const StrId text) {
    activity->note_.store(text);
    activity->hasNote_.store(true);
  };

  CatalogPage info;
  CatalogResult result = CatalogResult::Offline;
  const net::LinkResult link = stack.bringUp(kLoadLinkBudgetMs);
  if (link == net::LinkResult::Connected) {
    result =
        activity->catalog_->fetchPage(activity->requestedPage_, activity->entries_.capacity(), info, kFetchBudgetMs);
  }

  activity->entries_.clear();
  activity->onSd_.clear();
  Visitor visitor(*activity);
  const bool loaded = activity->catalog_->loadPage(activity->requestedPage_, info, visitor);
  if (loaded) {
    activity->page_ = activity->requestedPage_;
    activity->hasNext_ = info.hasNext;
    activity->hasPrevious_ = info.hasPrevious;
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
  activity->requestedView_.store(View::List);
  activity->viewChanged_.store(true, std::memory_order_release);
}

void BorgesLibraryActivity::startDownload() {
  downloading_ = entries_[focus_];
  downloadPercent_.store(0);
  shownPercent_.store(0);
  lastProgressPaint_ = millis();
  stack_->cancel.arm(kDownloadBudgetMs);
  setView(View::Downloading);
  if (!worker_.start(&BorgesLibraryActivity::downloadMain, this)) {
    note_.store(StrId::STR_BORGES_ERR_LOCAL);
    setView(View::DownloadFailed);
  }
}

void BorgesLibraryActivity::downloadMain(void* self) {
  auto* activity = static_cast<BorgesLibraryActivity*>(self);
  BorgesStack& stack = *activity->stack_;
  const auto done = [activity](const View view) {
    activity->requestedView_.store(view);
    activity->viewChanged_.store(true, std::memory_order_release);
  };
  const auto fail = [activity, &done](const StrId text) {
    activity->note_.store(text);
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
  switch (activity->catalog_->download(activity->downloading_, kBorgesBooksDir, activity->progress_.get(),
                                       kDownloadBudgetMs)) {
    case DownloadResult::Done:
    case DownloadResult::AlreadyPresent:
      library::invalidateLibraryIndex();
      for (size_t i = 0; i < activity->entries_.size(); ++i) {
        if (activity->catalog_->isOnSd(activity->entries_[i], kBorgesBooksDir)) activity->onSd_[i] = 1;
      }
      done(View::Downloaded);
      return;
    case DownloadResult::Interrupted:
      fail(StrId::STR_BORGES_LIB_INTERRUPTED);
      return;
    case DownloadResult::Cancelled:
      activity->requestedView_.store(View::List);
      activity->viewChanged_.store(true, std::memory_order_release);
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
  snprintf(path, sizeof(path), "%s/%s", kBorgesBooksDir, downloading_.fileName);
  stack_->bringDown();
  APP_STATE.openEpubPath = path;
  APP_STATE.saveToFile();
  silentRestartToReader(false);
}

void BorgesLibraryActivity::leave() {
  if (stack_) stack_->cancel.request();
  if (!worker_.waitForExit(4000)) {
    silentRestart();
    return;
  }
  stack_->bringDown();
  catalog_.reset();
  stack_.reset();
  auto status = makeUniqueNoThrow<BorgesActivity>(renderer, mappedInput, true, NetPayload::Status);
  if (status) {
    activityManager.replaceActivity(std::move(status));
  } else {
    silentRestart();
  }
}

void BorgesLibraryActivity::loop() {
  if (viewChanged_.exchange(false, std::memory_order_acquire)) {
    const View next = requestedView_.load();
    if (next == View::List && (focus_ >= static_cast<int>(entries_.size()) || wantLastRow_)) {
      focus_ = std::max(0, static_cast<int>(entries_.size()) - 1);
    }
    wantLastRow_ = false;
    worker_.reset();
    setView(next);
    return;
  }

  switch (view_) {
    case View::Loading:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) leave();
      return;
    case View::Downloading: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        stack_->cancel.request();
        return;
      }
      // E-ink: a bar that moves every few seconds, never one that flickers.
      const uint32_t percent = downloadPercent_.load(std::memory_order_relaxed);
      if (percent >= shownPercent_.load() + kProgressPaintStep && millis() - lastProgressPaint_ >= kProgressPaintMs) {
        shownPercent_.store(percent);
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
        startDownload();  // Picks up the .part where it stopped.
      }
      return;
    case View::List:
      break;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    leave();
    return;
  }
  const int count = static_cast<int>(entries_.size());
  if (count == 0) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) startLoad(page_);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (onSd_[focus_]) {
      downloading_ = entries_[focus_];
      downloadFocus_ = 0;
      setView(View::Downloaded);
    } else {
      startDownload();
    }
    return;
  }
  navigator_.onNextRelease([this, count] {
    if (focus_ + 1 >= count && hasNext_) {
      focus_ = 0;
      startLoad(page_ + 1);
      return;
    }
    focus_ = ButtonNavigator::nextIndex(focus_, count);
    requestUpdate();
  });
  navigator_.onPreviousRelease([this, count] {
    if (focus_ == 0 && hasPrevious_) {
      wantLastRow_ = true;
      startLoad(page_ - 1);
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

  if (entries_.empty()) {
    Rect text = area;
    text.x += padding;
    text.width -= padding * 2;
    const StrId message = hasNote_.load() ? note_.load() : StrId::STR_BORGES_LIB_EMPTY;
    drawWrappedCentered(renderer, text, UI_10_FONT_ID, area.y + area.height / 3, I18N.get(message), 3);
    return;
  }

  char size[16];
  for (size_t i = 0; i < entries_.size(); ++i) {
    const CatalogEntry& entry = entries_[i];
    const bool focused = static_cast<int>(i) == focus_;
    if (focused) renderer.fillRect(area.x, y, area.width, rowHeight, true);
    const int textWidth = area.width - padding * 2 - (onSd_[i] ? kCheckSize + 12 : 0);
    const std::string title = renderer.truncatedText(UI_10_FONT_ID, entry.title, textWidth);
    renderer.drawText(UI_10_FONT_ID, area.x + padding, y + 6, title.c_str(), !focused);

    formatSize(size, sizeof(size), entry.fileSize);
    std::string meta = entry.author;
    if (!meta.empty()) meta += " \xC2\xB7 ";
    meta += size;
    const std::string metaLine = renderer.truncatedText(SMALL_FONT_ID, meta.c_str(), textWidth);
    renderer.drawText(SMALL_FONT_ID, area.x + padding, y + 6 + renderer.getLineHeight(UI_10_FONT_ID), metaLine.c_str(),
                      !focused);
    if (onSd_[i]) {
      drawCheckMark(renderer, area.x + area.width - padding - kCheckSize, y + (rowHeight - kCheckSize) / 2, kCheckSize,
                    !focused);
    }
    y += rowHeight;
  }
  if (hasNote_.load()) {
    const int noteY = area.y + area.height - renderer.getLineHeight(SMALL_FONT_ID);
    if (noteY >= y) renderer.drawText(SMALL_FONT_ID, area.x + padding, noteY, I18N.get(note_.load()));
  }
}

void BorgesLibraryActivity::renderDownload() {
  Rect area = listArea(renderer);
  area.x += kSideMargin;
  area.width -= kSideMargin * 2;
  int y = area.y + kBlockGap * 2;
  y = drawWrapped(renderer, UI_12_FONT_ID, area.x, y, area.width, downloading_.title, 3, EpdFontFamily::BOLD);
  if (downloading_.author[0]) {
    y = drawWrapped(renderer, UI_10_FONT_ID, area.x, y + kLineGap, area.width, downloading_.author, 1);
  }
  y += kBlockGap * 2;

  if (view_ == View::Downloading) {
    y = drawWrapped(renderer, UI_10_FONT_ID, area.x, y, area.width, tr(STR_BORGES_LIB_DOWNLOADING), 1);
    y += kLineGap;
    const uint32_t percent = shownPercent_.load();
    drawProgressBar(renderer, area.x, y, area.width, 12, percent);
    y += 12 + kLineGap;
    char text[48];
    char total[16];
    formatSize(total, sizeof(total), downloading_.fileSize);
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
    y = drawWrapped(renderer, UI_10_FONT_ID, area.x, y, area.width, I18N.get(note_.load()), 3);
    y += kLineGap;
    drawWrapped(renderer, SMALL_FONT_ID, area.x, y, area.width, tr(STR_BORGES_LIB_RETRY_HINT), 2);
  }
}

void BorgesLibraryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  char subtitle[24] = {0};
  if (view_ == View::List && (hasNext_ || hasPrevious_)) {
    snprintf(subtitle, sizeof(subtitle), tr(STR_BORGES_LIB_PAGE_FMT), static_cast<unsigned>(page_));
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
      drawWrappedCentered(renderer, area, UI_10_FONT_ID, area.y + area.height / 3, tr(STR_BORGES_LIB_LOADING), 1);
      break;
    }
    case View::List:
      renderList();
      if (entries_.empty()) {
        confirm = tr(STR_SELECT);
      } else {
        confirm = tr(STR_SELECT);
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
      confirm = tr(STR_SELECT);
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
