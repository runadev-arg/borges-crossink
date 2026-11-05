#include "BorgesUi.h"

#include <Epub.h>
#include <Epub/BookMetadataCache.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/reader/EpubReaderUtils.h"
#include "fontIds.h"
#include "net/NetFailure.h"

namespace borges {
namespace glue {

std::string bookTitleFor(const std::string& path) {
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (book.path == path && !book.title.empty()) return book.title;
  }
  const size_t slash = path.find_last_of('/');
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0) name.resize(dot);
  return name;
}

float localPercentFor(const std::string& path) {
  // The same sum as the reader's own status bar: pages left behind, not the page on screen,
  // or the prompt says 1% about a book the reader shows at 0%. The Home list's cached number
  // counts the current page as read.
  const std::string cachePath = Epub::resolveCachePathForFilePath(path, "/.crosspoint");
  EpubReaderUtils::Progress progress;
  if ((!EpubReaderUtils::readProgressFile("BORGES", cachePath + "/progress.bin", progress) &&
       !EpubReaderUtils::readProgressFile("BORGES", cachePath + "/progress.bin.bak", progress)) ||
      !progress.hasPageCount || progress.pageCount <= 0) {
    return -1.0f;
  }
  BookMetadataCache metadata(cachePath);
  if (!metadata.load() || progress.spineIndex < 0 || progress.spineIndex >= metadata.getSpineCount()) return -1.0f;
  const size_t bookSize = metadata.getSpineCumulativeSize(metadata.getSpineCount() - 1);
  if (bookSize == 0) return -1.0f;
  const size_t before = progress.spineIndex >= 1 ? metadata.getSpineCumulativeSize(progress.spineIndex - 1) : 0;
  const size_t chapter = metadata.getSpineCumulativeSize(progress.spineIndex) - before;
  const float read = static_cast<float>(progress.pageNumber) / static_cast<float>(progress.pageCount);
  return (static_cast<float>(before) + static_cast<float>(chapter) * read) / static_cast<float>(bookSize) * 100.0f;
}

int drawWrapped(const GfxRenderer& renderer, const int fontId, const int x, int y, const int width, const char* text,
                const int maxLines, const EpdFontFamily::Style style) {
  if (!text || !text[0]) return y;
  const int lineHeight = renderer.getLineHeight(fontId);
  for (const auto& line : renderer.wrappedText(fontId, text, width, maxLines, style)) {
    renderer.drawText(fontId, x, y, line.c_str(), true, style);
    y += lineHeight;
  }
  return y;
}

int drawWrappedCentered(const GfxRenderer& renderer, const Rect& rect, const int fontId, int y, const char* text,
                        const int maxLines, const EpdFontFamily::Style style) {
  if (!text || !text[0]) return y;
  const int lineHeight = renderer.getLineHeight(fontId);
  for (const auto& line : renderer.wrappedText(fontId, text, rect.width, maxLines, style)) {
    const int lineWidth = renderer.getTextWidth(fontId, line.c_str(), style);
    renderer.drawText(fontId, rect.x + (rect.width - lineWidth) / 2, y, line.c_str(), true, style);
    y += lineHeight;
  }
  return y;
}

int actionRowHeight(const GfxRenderer& renderer) { return renderer.getLineHeight(UI_10_FONT_ID) + 20; }

void drawActionRow(const GfxRenderer& renderer, const int x, const int y, const int width, const int height,
                   const char* label, const bool focused) {
  if (focused) renderer.fillRect(x, y, width, height, true);
  const std::string text = renderer.truncatedText(UI_10_FONT_ID, label, width - 24);
  const int textY = y + (height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  renderer.drawText(UI_10_FONT_ID, x + 12, textY, text.c_str(), !focused);
}

int drawLabelValue(const GfxRenderer& renderer, const int x, const int y, const int width, const char* label,
                   const char* value) {
  const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, value, EpdFontFamily::BOLD);
  const int labelMax = width - valueWidth - 16;
  if (labelMax > 40) {
    const std::string left = renderer.truncatedText(UI_10_FONT_ID, label, labelMax);
    renderer.drawText(UI_10_FONT_ID, x, y, left.c_str());
    renderer.drawText(UI_10_FONT_ID, x + width - valueWidth, y, value, true, EpdFontFamily::BOLD);
    return y + renderer.getLineHeight(UI_10_FONT_ID);
  }
  // A long device name takes the line: label above, value below, both whole.
  renderer.drawText(UI_10_FONT_ID, x, y, renderer.truncatedText(UI_10_FONT_ID, label, width).c_str());
  const int next = y + renderer.getLineHeight(UI_10_FONT_ID);
  renderer.drawText(UI_10_FONT_ID, x, next,
                    renderer.truncatedText(UI_10_FONT_ID, value, width, EpdFontFamily::BOLD).c_str(), true,
                    EpdFontFamily::BOLD);
  return next + renderer.getLineHeight(UI_10_FONT_ID);
}

void drawHairline(const GfxRenderer& renderer, const int x, const int y, const int width) {
  renderer.drawLine(x, y, x + width - 1, y, true);
}

void drawCheckMark(const GfxRenderer& renderer, const int x, const int y, const int size, const bool black) {
  const int midX = x + size / 3;
  const int bottomY = y + size - 1;
  renderer.drawLine(x, y + size / 2, midX, bottomY, 2, black);
  renderer.drawLine(midX, bottomY, x + size - 1, y, 2, black);
}

void drawProgressBar(const GfxRenderer& renderer, const int x, const int y, const int width, const int height,
                     const uint32_t percent) {
  renderer.drawRect(x, y, width, height, true);
  const uint32_t clamped = percent > 100 ? 100 : percent;
  const int fill = static_cast<int>((width - 4) * clamped / 100);
  if (fill > 0) renderer.fillRect(x + 2, y + 2, fill, height - 4, true);
}

void formatPercent(char* out, const size_t cap, const uint32_t pctMicro, const bool oneDecimal) {
  if (oneDecimal) {
    const uint32_t tenths = (pctMicro + 50000) / 100000;
    const char separator = I18N.getLanguage() == Language::ES ? ',' : '.';
    snprintf(out, cap, "%u%c%u%%", static_cast<unsigned>(tenths / 10), separator, static_cast<unsigned>(tenths % 10));
  } else {
    snprintf(out, cap, "%u%%", static_cast<unsigned>((pctMicro + 500000) / 1000000));
  }
}

void formatAge(char* out, const size_t cap, const Age& age) {
  switch (age.bucket) {
    case AgeBucket::JustNow:
      snprintf(out, cap, "%s", tr(STR_BORGES_JUST_NOW));
      return;
    case AgeBucket::Minutes:
      snprintf(out, cap, tr(STR_BORGES_MINUTES_AGO), static_cast<int>(age.value));
      return;
    case AgeBucket::Hours:
      snprintf(out, cap, tr(STR_BORGES_HOURS_AGO), static_cast<int>(age.value));
      return;
    case AgeBucket::Days:
      if (age.value == 1) {
        snprintf(out, cap, "%s", tr(STR_BORGES_YESTERDAY));
      } else {
        snprintf(out, cap, tr(STR_BORGES_DAYS_AGO), static_cast<int>(age.value));
      }
      return;
    case AgeBucket::Unknown:
      break;
  }
  if (cap) out[0] = '\0';
}

void formatSize(char* out, const size_t cap, const uint64_t bytes) {
  const auto kib = static_cast<unsigned>(bytes / 1024);
  if (kib < 1024) {
    snprintf(out, cap, "%u KB", kib);
  } else {
    const char separator = I18N.getLanguage() == Language::ES ? ',' : '.';
    snprintf(out, cap, "%u%c%u MB", kib / 1024, separator, (kib % 1024) * 10 / 1024);
  }
}

StrId describeLink(const net::LinkResult result) {
  switch (result) {
    case net::LinkResult::NoSavedNetworks:
      return StrId::STR_BORGES_ERR_NO_WIFI;
    case net::LinkResult::NotFound:
      return StrId::STR_BORGES_ERR_WIFI_RANGE;
    case net::LinkResult::AuthFailed:
      return StrId::STR_BORGES_ERR_WIFI_AUTH;
    case net::LinkResult::Timeout:
    case net::LinkResult::Cancelled:
      return StrId::STR_BORGES_ERR_TIMEOUT;
    case net::LinkResult::RadioError:
    case net::LinkResult::Connected:
      break;
  }
  return StrId::STR_BORGES_ERR_RADIO;
}

StrId describeOutcome(const SessionOutcome outcome) {
  switch (outcome) {
    case SessionOutcome::Ok:
    case SessionOutcome::AwaitingChoice:
      return StrId::STR_BORGES_SYNC_OK;
    case SessionOutcome::NotPaired:
    case SessionOutcome::NeedsRelink:
      return StrId::STR_BORGES_ERR_RELINK;
    case SessionOutcome::Cancelled:
      return StrId::STR_BORGES_SYNC_CANCELLED;
    case SessionOutcome::BudgetExhausted:
      return StrId::STR_BORGES_ERR_TIMEOUT;
    case SessionOutcome::RateLimited:
      return StrId::STR_BORGES_ERR_RATE;
    case SessionOutcome::Transport:
      return StrId::STR_BORGES_ERR_TRANSPORT;
    case SessionOutcome::ServerError:
    case SessionOutcome::ProtocolError:
    case SessionOutcome::StaleChoice:
      return StrId::STR_BORGES_ERR_SERVER;
    case SessionOutcome::LocalError:
      return StrId::STR_BORGES_ERR_LOCAL;
  }
  return StrId::STR_BORGES_ERR_SERVER;
}

StrId describeDiagnostics(const Diagnostics& diagnostics) {
  const char* code = diagnostics.code;
  if (diagnostics.httpStatus == 401 || diagnostics.httpStatus == 403 || strcmp(code, "device_revoked") == 0) {
    return StrId::STR_BORGES_ERR_RELINK;
  }
  if (diagnostics.httpStatus == 429 || strcmp(code, "rate_limited") == 0) return StrId::STR_BORGES_ERR_RATE;
  if (strcmp(code, "transport") == 0) {
    const char* failure =
        net::failureCode(diagnostics.transportStage, diagnostics.transportCode, TimePrecision::Unknown);
    if (failure && strcmp(failure, "tls_clock") == 0) return StrId::STR_BORGES_ERR_CLOCK;
    if (failure && strcmp(failure, "tls_timeout") == 0) return StrId::STR_BORGES_ERR_TIMEOUT;
    return StrId::STR_BORGES_ERR_TRANSPORT;
  }
  if (strcmp(code, "low_memory") == 0) return StrId::STR_BORGES_ERR_TRANSPORT;
  if (strcmp(code, "no_wifi") == 0) return StrId::STR_BORGES_ERR_NO_WIFI;
  if (strcmp(code, "wifi_auth") == 0) return StrId::STR_BORGES_ERR_WIFI_AUTH;
  if (strcmp(code, "wifi_timeout") == 0) return StrId::STR_BORGES_ERR_TIMEOUT;
  if (strstr(code, "_write") || strstr(code, "_read") || strstr(code, "_io")) return StrId::STR_BORGES_ERR_LOCAL;
  return StrId::STR_BORGES_ERR_SERVER;
}

}  // namespace glue
}  // namespace borges
