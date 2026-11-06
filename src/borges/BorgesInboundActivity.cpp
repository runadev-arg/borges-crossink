#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "BorgesActivities.h"
#include "BorgesUi.h"
#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace borges {
namespace glue {

namespace {

constexpr int kRowPadY = 10;
constexpr int kMetaGap = 4;
constexpr int kNoteGap = 6;
// Three lines read as a passage, and six of them fit a page. Across, the lines are long enough that
// two say as much, and a third would leave three rows on the glass.
constexpr int kLinesPortrait = 3;
constexpr int kLinesLandscape = 2;

int textLines(const GfxRenderer& renderer) {
  return renderer.getScreenHeight() > renderer.getScreenWidth() ? kLinesPortrait : kLinesLandscape;
}

// Every row the same height whatever its text: the focus moves by a fixed step and a page keeps its
// shape, which is worth more on this panel than the space a short highlight leaves empty.
int rowHeight(const GfxRenderer& renderer) {
  return renderer.getLineHeight(UI_10_FONT_ID) * textLines(renderer) + kMetaGap +
         renderer.getLineHeight(SMALL_FONT_ID) + kRowPadY * 2;
}

Rect headerRect(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  return Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight};
}

Rect listArea(const GfxRenderer& renderer) {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header = headerRect(renderer);
  const int top = header.y + header.height + kLineGap;
  return Rect{safe.x, top, safe.width, safe.y + safe.height - top};
}

int noteHeight(const GfxRenderer& renderer) { return renderer.getLineHeight(SMALL_FONT_ID) + kNoteGap; }

// A passage comes with the line breaks of wherever it was read. On a row they mean nothing.
void collapseSpaces(char* text) {
  char* out = text;
  bool space = true;
  for (const char* p = text; *p; ++p) {
    const auto c = static_cast<unsigned char>(*p);
    if (c == 0xC2 && static_cast<unsigned char>(p[1]) == 0xA0) {
      ++p;
      if (!space) *out++ = ' ';
      space = true;
    } else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      if (!space) *out++ = ' ';
      space = true;
    } else {
      *out++ = *p;
      space = false;
    }
  }
  if (out > text && out[-1] == ' ') --out;
  *out = '\0';
}

}  // namespace

BorgesInboundActivity::BorgesInboundActivity(GfxRenderer& renderer, MappedInputManager& input, std::string bookPath)
    : Activity("BorgesInbound", renderer, input), bookPath_(std::move(bookPath)) {}

BorgesInboundActivity::~BorgesInboundActivity() = default;

void BorgesInboundActivity::onEnter() {
  Activity::onEnter();
  list_ = makeUniqueNoThrow<InboundList>(fs_);
  row_ = makeUniqueNoThrow<InboundHighlight>();
  // The store and the clock only for this moment: this screen reads the log, and the log only.
  auto stack = makeStack();
  BookId id;
  unreadable_ = true;
  if (list_ && row_ && stack && stack->openStore() && partialMd5(stack->fs, bookPath_.c_str(), id)) {
    now_ = stack->clock.wallNow();
    const uint16_t epoch = stack->store.state().accountEpoch;
    stack.reset();
    auto own = makeOwnPassages();
    unreadable_ = !list_->load(id, epoch, own.get());
  }
  if (unreadable_) LOG_ERR("BORGES", "Could not read this book's highlights");
  requestUpdate();
}

void BorgesInboundActivity::onExit() {
  list_.reset();
  row_.reset();
  Activity::onExit();
}

int BorgesInboundActivity::rowsPerPage() const {
  int height = listArea(renderer).height;
  if (list_ && list_->partial()) height -= noteHeight(renderer);
  return std::max(1, height / rowHeight(renderer));
}

void BorgesInboundActivity::go() {
  if (!list_ || !row_ || !list_->read(static_cast<uint16_t>(focus_), *row_)) {
    readFailed_ = true;
    requestUpdate();
    return;
  }
  BorgesHighlightJumpResult result;
  result.xpointer = row_->xpointer;
  result.hasPct = row_->hasPct;
  result.pctMicro = row_->pctMicro;
  setResult(std::move(result));
  finish();
}

void BorgesInboundActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  const int total = list_ ? list_->count() : 0;
  if (total == 0) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    go();
    return;
  }
  const auto moveTo = [this](const int next) {
    focus_ = next;
    readFailed_ = false;
    requestUpdate();
  };
  navigator_.onNextRelease([&] { moveTo(ButtonNavigator::nextIndex(focus_, total)); });
  navigator_.onPreviousRelease([&] { moveTo(ButtonNavigator::previousIndex(focus_, total)); });
  const int perPage = rowsPerPage();
  navigator_.onNextContinuous([&] { moveTo(ButtonNavigator::nextPageIndex(focus_, total, perPage)); });
  navigator_.onPreviousContinuous([&] { moveTo(ButtonNavigator::previousPageIndex(focus_, total, perPage)); });
}

// The record is in row_.
void BorgesInboundActivity::drawRow(const int y, const int width, const int height, const int lines,
                                    const bool focused) {
  const Rect area = listArea(renderer);
  if (focused) renderer.fillRect(area.x, y, area.width, height, true);
  const int x = area.x + kSideMargin;

  collapseSpaces(row_->text);
  if (row_->truncated) {
    // Cut for being long: it says so even when what is left would fit the row.
    const size_t used = strlen(row_->text);
    if (used + 4 < sizeof(row_->text)) memcpy(row_->text + used, "\xE2\x80\xA6", 4);
  }
  int lineY = y + kRowPadY;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, row_->text, width, lines, EpdFontFamily::REGULAR)) {
    renderer.drawText(UI_10_FONT_ID, x, lineY, line.c_str(), !focused);
    lineY += lineHeight;
  }
  // Under its own text whatever the length: what is said about a passage belongs next to it, and
  // the room a short one leaves goes below.

  // Who and when stay whole. The chapter takes what is left, and is the one to give way.
  constexpr const char* kSeparator = " \xC2\xB7 ";
  char age[32] = {0};
  if (row_->timeEpoch > 0) formatAge(age, sizeof(age), ageOf(row_->timeEpoch, now_));
  std::string tail;
  if (row_->device[0]) tail = renderer.truncatedText(SMALL_FONT_ID, row_->device, width / 2);
  if (age[0]) {
    if (!tail.empty()) tail += kSeparator;
    tail += age;
  }
  std::string meta;
  const int tailWidth = tail.empty() ? 0 : renderer.getTextWidth(SMALL_FONT_ID, tail.c_str());
  const int separatorWidth = tail.empty() ? 0 : renderer.getTextWidth(SMALL_FONT_ID, kSeparator);
  if (row_->chapter[0] && width - tailWidth - separatorWidth > renderer.getLineHeight(SMALL_FONT_ID) * 3) {
    meta = renderer.truncatedText(SMALL_FONT_ID, row_->chapter, width - tailWidth - separatorWidth);
    if (!tail.empty()) meta += kSeparator;
  }
  meta += tail;
  if (!meta.empty()) renderer.drawText(SMALL_FONT_ID, x, lineY + kMetaGap, meta.c_str(), !focused);
}

void BorgesInboundActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const int total = list_ ? list_->count() : 0;
  const int perPage = rowsPerPage();
  const int page = total ? focus_ / perPage : 0;
  const int pages = total ? (total + perPage - 1) / perPage : 1;

  char count[16] = {0};
  if (pages > 1) snprintf(count, sizeof(count), "%d/%d", page + 1, pages);
  GUI.drawHeader(renderer, headerRect(renderer), tr(STR_BORGES_INBOUND), count[0] ? count : nullptr, true);

  const Rect area = listArea(renderer);
  const int width = area.width - kSideMargin * 2;
  if (total == 0) {
    Rect text = area;
    text.x += kSideMargin;
    text.width = width;
    drawWrappedCentered(renderer, text, UI_10_FONT_ID, area.y + area.height / 3,
                        unreadable_ ? tr(STR_BORGES_INBOUND_UNREADABLE) : tr(STR_BORGES_INBOUND_EMPTY), 3);
  } else {
    const int height = rowHeight(renderer);
    const int lines = textLines(renderer);
    int y = area.y;
    for (int i = page * perPage; i < total && i < (page + 1) * perPage; ++i) {
      if (row_ && list_->read(static_cast<uint16_t>(i), *row_)) drawRow(y, width, height, lines, i == focus_);
      y += height;
    }
    // At the foot and only when it is true, as the library says it is showing an older list.
    const char* note = readFailed_        ? tr(STR_BORGES_INBOUND_FAILED)
                       : list_->partial() ? tr(STR_BORGES_INBOUND_PARTIAL)
                                          : nullptr;
    if (note) {
      const std::string line = renderer.truncatedText(SMALL_FONT_ID, note, width);
      renderer.drawText(SMALL_FONT_ID, area.x + kSideMargin, area.y + area.height - noteHeight(renderer) + kNoteGap,
                        line.c_str());
    }
  }

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), total ? tr(STR_BORGES_INBOUND_GO) : "",
                            total ? tr(STR_DIR_UP) : "", total ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  // Moving the focus changes two rows: fast is clean enough. A new page changes all of them.
  renderer.displayBuffer(page == shownPage_ ? HalDisplay::FAST_REFRESH : HalDisplay::HALF_REFRESH);
  shownPage_ = page;
}

}  // namespace glue
}  // namespace borges
