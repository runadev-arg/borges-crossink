#pragma once

// The handful of drawing and wording decisions every Borges screen shares. E-ink rules:
// hierarchy from size and weight, not boxes; focus is one filled row; nothing moves; a
// screen repaints only when what it says has changed.

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "Store.h"
#include "SyncEngine.h"
#include "borges/logic/GlueState.h"
#include "borges/net/BorgesLink.h"
#include "components/themes/BaseTheme.h"

namespace borges {
namespace glue {

constexpr int kSideMargin = 24;
constexpr int kBlockGap = 20;
constexpr int kLineGap = 6;

// A book's title from the recent list, else its file name without the extension.
std::string bookTitleFor(const std::string& path);
// What this device knows of its place in the book before loading it. Negative: unknown.
float localPercentFor(const std::string& path);

// Left-aligned word wrap. Returns the y below the text.
int drawWrapped(const GfxRenderer& renderer, int fontId, int x, int y, int width, const char* text, int maxLines,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR);
// Same, centered on rect.
int drawWrappedCentered(const GfxRenderer& renderer, const Rect& rect, int fontId, int y, const char* text,
                        int maxLines, EpdFontFamily::Style style = EpdFontFamily::REGULAR);

// One full-width action. Focus is the row filled, the text reversed.
void drawActionRow(const GfxRenderer& renderer, int x, int y, int width, int height, const char* label, bool focused);
int actionRowHeight(const GfxRenderer& renderer);

// Label on the left, value on the right on the same baseline. Returns the y below.
int drawLabelValue(const GfxRenderer& renderer, int x, int y, int width, const char* label, const char* value);
void drawHairline(const GfxRenderer& renderer, int x, int y, int width);
// The picker left a connection. Nothing to do on a device, where the radio simply is up.
void noteNetworkPicked(IFs& fs);
// The book opened without learning whether someone read further. Over the page, until it turns.
void drawCheckNotice(const GfxRenderer& renderer);
// Drawn, not typed: no UI font is guaranteed to carry the glyph.
void drawCheckMark(const GfxRenderer& renderer, int x, int y, int size, bool black);
void drawProgressBar(const GfxRenderer& renderer, int x, int y, int width, int height, uint32_t percent);

// "34 %" in the way the language writes it, with one decimal when asked to.
void formatPercent(char* out, size_t cap, uint32_t pctMicro, bool oneDecimal);
void formatAge(char* out, size_t cap, const Age& age);
void formatSize(char* out, size_t cap, uint64_t bytes);

// Plain words. A stored error never reaches the screen as a code.
StrId describeLink(net::LinkResult result);
StrId describeOutcome(SessionOutcome outcome);
StrId describeDiagnostics(const Diagnostics& diagnostics);

}  // namespace glue
}  // namespace borges
