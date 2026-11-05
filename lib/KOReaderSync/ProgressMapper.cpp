#include "ProgressMapper.h"

#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ChapterXPathResolver.h"
#include "Epub/Section.h"

namespace {
int parseIndex(const std::string& xpath, const char* prefix, bool last = false) {
  const size_t prefixLen = strlen(prefix);
  const size_t pos = last ? xpath.rfind(prefix) : xpath.find(prefix);
  if (pos == std::string::npos) return -1;
  const size_t numStart = pos + prefixLen;
  const size_t numEnd = xpath.find(']', numStart);
  if (numEnd == std::string::npos || numEnd == numStart) return -1;
  int val = 0;
  for (size_t i = numStart; i < numEnd; i++) {
    if (xpath[i] < '0' || xpath[i] > '9') return -1;
    val = val * 10 + (xpath[i] - '0');
  }
  return val;
}

// Parsed representation of one step in the XPath ancestry.
struct XPathStep {
  char tag[12];      // element name, null-terminated
  int siblingIndex;  // 1-based sibling index, or 0 if unspecified (treat as 1)
};

static constexpr int MAX_XPATH_DEPTH = 16;

// Parse the XPath segment between /body/DocFragment[N]/body/ and the terminal position
// into an ordered sequence of steps. Returns step count, 0 on failure.
// Example input: "/body/DocFragment[1]/body/div[1]/ul/li[4]/text()[1].51"
// Fills steps with: {div,1}, {ul,1}, {li,4}
int parseXPathSteps(const std::string& xpath, XPathStep steps[MAX_XPATH_DEPTH]) {
  static const char kBodyFrag[] = "/body/DocFragment[";
  const size_t fragPos = xpath.find(kBodyFrag);
  if (fragPos == std::string::npos) return 0;
  const size_t afterBracket = xpath.find(']', fragPos + strlen(kBodyFrag));
  if (afterBracket == std::string::npos) return 0;
  static const char kBody[] = "/body/";
  if (xpath.compare(afterBracket + 1, strlen(kBody), kBody) != 0) return 0;
  size_t pos = afterBracket + 1 + strlen(kBody);

  size_t stepsEnd = xpath.rfind("/text()");
  if (stepsEnd == std::string::npos) {
    const size_t dot = xpath.rfind('.');
    if (dot != std::string::npos && dot > pos) {
      if (dot + 1 >= xpath.size()) return 0;
      for (size_t i = dot + 1; i < xpath.size(); i++) {
        if (xpath[i] < '0' || xpath[i] > '9') return 0;
      }
      stepsEnd = dot;
    } else {
      // KOReader also accepts a structural XPath ending at the element itself.
      // ChapterXPathResolver emits this form for a zero character offset.
      stepsEnd = xpath.size();
    }
  }
  if (stepsEnd <= pos) return 0;

  int count = 0;
  while (pos < stepsEnd && count < MAX_XPATH_DEPTH) {
    const size_t slash = xpath.find('/', pos);
    const size_t segEnd = (slash < stepsEnd) ? slash : stepsEnd;

    XPathStep& step = steps[count];
    const size_t bracket = xpath.find('[', pos);
    const size_t nameEnd = (bracket != std::string::npos && bracket < segEnd) ? bracket : segEnd;
    const size_t nameLen = nameEnd - pos;
    if (nameLen == 0 || nameLen >= sizeof(step.tag)) return 0;
    memcpy(step.tag, xpath.c_str() + pos, nameLen);
    step.tag[nameLen] = '\0';

    if (bracket != std::string::npos && bracket < segEnd) {
      const size_t closeBracket = xpath.find(']', bracket + 1);
      if (closeBracket == std::string::npos || closeBracket > segEnd) return 0;
      int idx = 0;
      for (size_t i = bracket + 1; i < closeBracket; i++) {
        if (xpath[i] < '0' || xpath[i] > '9') return 0;
        idx = idx * 10 + (xpath[i] - '0');
      }
      step.siblingIndex = idx;
    } else {
      step.siblingIndex = 1;
    }

    count++;
    pos = (slash < stepsEnd) ? slash + 1 : stepsEnd;
  }
  return count;
}

bool rewriteDocFragment(std::string& xpath, const int spineIndex) {
  static constexpr char kPrefix[] = "/body/DocFragment[";
  const size_t start = xpath.find(kPrefix);
  if (start == std::string::npos) return false;
  const size_t numberStart = start + strlen(kPrefix);
  const size_t numberEnd = xpath.find(']', numberStart);
  if (numberEnd == std::string::npos || spineIndex < 0) return false;
  xpath.replace(numberStart, numberEnd - numberStart, std::to_string(spineIndex + 1));
  return true;
}

bool rewriteXPathStepIndex(std::string& xpath, const int ordinal, const int siblingIndex) {
  if (ordinal < 0 || siblingIndex <= 0) return false;
  static constexpr char kBody[] = "]/body/";
  const size_t body = xpath.find(kBody);
  if (body == std::string::npos) return false;
  size_t segmentStart = body + strlen(kBody);
  for (int current = 0; current <= ordinal; ++current) {
    size_t segmentEnd = xpath.find('/', segmentStart);
    if (segmentEnd == std::string::npos) segmentEnd = xpath.size();
    const size_t dot = xpath.find('.', segmentStart);
    if (dot != std::string::npos && dot < segmentEnd) segmentEnd = dot;
    if (segmentEnd <= segmentStart) return false;
    if (current == ordinal) {
      const size_t bracket = xpath.find('[', segmentStart);
      if (bracket != std::string::npos && bracket < segmentEnd) {
        const size_t close = xpath.find(']', bracket + 1);
        if (close == std::string::npos || close > segmentEnd) return false;
        xpath.replace(bracket + 1, close - bracket - 1, std::to_string(siblingIndex));
      } else {
        xpath.insert(segmentEnd, "[" + std::to_string(siblingIndex) + "]");
      }
      return true;
    }
    if (segmentEnd >= xpath.size() || xpath[segmentEnd] != '/') return false;
    segmentStart = segmentEnd + 1;
  }
  return false;
}

bool mapCurrentXPathToSource(const std::shared_ptr<Epub>& epub, const int currentSpineIndex, std::string& xpath) {
  if (!epub->hasSourceSpineMap()) return !epub->requiresSourceSpineMap();
  Epub::SourceSpineMapEntry entry;
  if (!epub->getSourceSpineMapEntry(currentSpineIndex, entry) || !rewriteDocFragment(xpath, entry.sourceSpineIndex)) {
    return false;
  }
  if (entry.rangeCount == 0) return true;

  XPathStep steps[MAX_XPATH_DEPTH];
  const int stepCount = parseXPathSteps(xpath, steps);
  if (entry.containerDepth >= stepCount) return false;
  XPathStep& child = steps[entry.containerDepth];
  for (size_t i = 0; i < entry.rangeCount; ++i) {
    const Epub::SourceChildRange* range = epub->getSourceChildRange(entry, i);
    if (!range || strcasecmp(range->name, child.tag) != 0 || child.siblingIndex <= 0 ||
        child.siblingIndex > range->count) {
      continue;
    }
    return rewriteXPathStepIndex(xpath, entry.containerDepth, child.siblingIndex + range->offset);
  }
  return false;
}

bool mapSourceXPathToCurrent(const std::shared_ptr<Epub>& epub, std::string& xpath, int& currentSpineIndex) {
  if (!epub->hasSourceSpineMap()) return !epub->requiresSourceSpineMap();
  const int sourceSpineIndex = parseIndex(xpath, "/body/DocFragment[") - 1;
  if (sourceSpineIndex < 0) return false;

  XPathStep steps[MAX_XPATH_DEPTH];
  const int stepCount = parseXPathSteps(xpath, steps);
  uint16_t currentSiblingIndex = 0;
  bool mapped = false;
  int matchedDepth = -1;
  for (int depth = 0; depth < stepCount && !mapped; ++depth) {
    mapped = epub->findCurrentSpineForSource(sourceSpineIndex, static_cast<uint8_t>(depth), steps[depth].tag,
                                             static_cast<uint16_t>(steps[depth].siblingIndex), currentSpineIndex,
                                             currentSiblingIndex);
    if (mapped) matchedDepth = depth;
  }
  if (!mapped && stepCount == 0) {
    mapped = epub->findCurrentSpineForSource(sourceSpineIndex, 0, nullptr, 0, currentSpineIndex, currentSiblingIndex);
  }
  if (!mapped || !rewriteDocFragment(xpath, currentSpineIndex)) return false;
  return matchedDepth < 0 || rewriteXPathStepIndex(xpath, matchedDepth, currentSiblingIndex);
}
}  // namespace

KOReaderPosition ProgressMapper::toKOReader(const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos,
                                            const PositionCoordinateSpace coordinateSpace) {
  KOReaderPosition result;
  float intra =
      (pos.totalPages > 1) ? static_cast<float>(pos.pageNumber) / static_cast<float>(pos.totalPages - 1) : 0.0f;
  result.percentage = epub->calculateProgress(pos.spineIndex, intra);
  uint8_t minimumPathDepth = 0;
  Epub::SourceSpineMapEntry sourceEntry;
  if (coordinateSpace == PositionCoordinateSpace::SourceDocument &&
      epub->getSourceSpineMapEntry(pos.spineIndex, sourceEntry) && sourceEntry.rangeCount > 0) {
    minimumPathDepth = sourceEntry.containerDepth + 1;
  }
  if (pos.hasVisibleTextOffset) {
    result.xpath = ChapterXPathResolver::findXPathForVisibleTextOffset(epub, pos.spineIndex, pos.visibleTextOffset,
                                                                       minimumPathDepth);
  }
  if (result.xpath.empty() && pos.hasLiIndex && pos.liIndex > 0) {
    result.xpath = ChapterXPathResolver::findXPathForListItem(epub, pos.spineIndex, pos.liIndex);
  }
  if (result.xpath.empty() && pos.hasParagraphIndex && pos.paragraphIndex > 0) {
    result.xpath = ChapterXPathResolver::findXPathForParagraph(epub, pos.spineIndex, pos.paragraphIndex);
  }
  // Fall back to progress-based XPath, then to the start of the chapter.
  if (result.xpath.empty()) {
    result.xpath = ChapterXPathResolver::findXPathForProgress(epub, pos.spineIndex, intra, minimumPathDepth);
  }
  if (result.xpath.empty()) {
    result.xpath = ChapterXPathResolver::bodyXPath(epub, pos.spineIndex);
  }
  if (coordinateSpace == PositionCoordinateSpace::SourceDocument &&
      !mapCurrentXPathToSource(epub, pos.spineIndex, result.xpath)) {
    LOG_ERR("PM", "Source position map unavailable for optimized spine %d; re-optimize the EPUB", pos.spineIndex);
    result.xpath.clear();
    result.valid = false;
    return result;
  }
  LOG_DBG("PM", "-> KO: spine=%d page=%d/%d %.2f%% %s", pos.spineIndex, pos.pageNumber, pos.totalPages,
          result.percentage * 100, result.xpath.c_str());
  return result;
}

std::optional<CrossPointPosition> ProgressMapper::fromRichPosition(const std::shared_ptr<Epub>& epub,
                                                                   const KOReaderRichPosition& rich,
                                                                   GfxRenderer& renderer) {
  const int spineCount = epub->getSpineItemsCount();
  if (static_cast<int>(rich.spineIndex) >= spineCount) {
    LOG_DBG("PM", "Rich position spine %u out of range (%d spine items)", rich.spineIndex, spineCount);
    return std::nullopt;
  }

  CrossPointPosition result{};
  result.spineIndex = rich.spineIndex;

  Section tempSection(epub, result.spineIndex, renderer);
  const auto cachedCount = tempSection.getCachedPageCount();
  if (!cachedCount || *cachedCount <= 0) {
    // No local layout for the target spine yet; the percentage/xpath mapping
    // handles density estimation better than a blind copy of remote pages.
    LOG_DBG("PM", "Rich position spine %u has no cached page count", rich.spineIndex);
    return std::nullopt;
  }
  result.totalPages = *cachedCount;

  const int remotePages = rich.totalPages > 0 ? rich.totalPages : 1;
  if (result.totalPages == remotePages) {
    // Identical layout (same render settings) — the page transfers losslessly.
    result.pageNumber = std::min<int>(rich.pageNumber, result.totalPages - 1);
    LOG_DBG("PM", "Rich position exact: spine=%d page=%d/%d", result.spineIndex, result.pageNumber, result.totalPages);
    return result;
  }

  // Layout differs; the paragraph LUT is the most accurate anchor we have.
  if (rich.paragraphIndex.has_value()) {
    const auto lutPage = tempSection.getPageForParagraphIndex(*rich.paragraphIndex);
    if (lutPage.has_value()) {
      result.paragraphIndex = *rich.paragraphIndex;
      result.hasParagraphIndex = true;
      result.pageNumber = std::min<int>(*lutPage, result.totalPages - 1);
      LOG_DBG("PM", "Rich position para %u -> spine=%d page=%d/%d", *rich.paragraphIndex, result.spineIndex,
              result.pageNumber, result.totalPages);
      return result;
    }
  }

  // Fall back to the intra-spine page fraction.
  const float intra =
      (remotePages > 1) ? static_cast<float>(rich.pageNumber) / static_cast<float>(remotePages - 1) : 0.0f;
  result.pageNumber = std::max(
      0, std::min(static_cast<int>(intra * static_cast<float>(result.totalPages - 1) + 0.5f), result.totalPages - 1));
  LOG_DBG("PM", "Rich position scaled: spine=%d remote %u/%d -> page=%d/%d", result.spineIndex, rich.pageNumber,
          remotePages, result.pageNumber, result.totalPages);
  return result;
}

CrossPointPosition ProgressMapper::toCrossPoint(const std::shared_ptr<Epub>& epub, const KOReaderPosition& koPos,
                                                int currentSpineIndex, int totalPagesInCurrentSpine,
                                                const PositionCoordinateSpace coordinateSpace) {
  CrossPointPosition result{};
  KOReaderPosition mappedKoPos = koPos;
  if (coordinateSpace == PositionCoordinateSpace::SourceDocument) {
    int mappedSpineIndex = -1;
    if (!mapSourceXPathToCurrent(epub, mappedKoPos.xpath, mappedSpineIndex)) {
      LOG_ERR("PM", "Cannot map source XPath into this optimized EPUB; re-optimize the EPUB");
      result.valid = false;
      return result;
    }
  }
  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) return result;

  const int spineCount = epub->getSpineItemsCount();
  const float clampedPercentage = std::max(0.0f, std::min(1.0f, koPos.percentage));
  const size_t targetBytes = static_cast<size_t>(static_cast<float>(bookSize) * clampedPercentage);

  // Since DOM version 20240114 crengine makes a DocFragment for every spine item, so the numbers match.
  // A document with a single spine item writes the fragment without an index.
  const int docFrag = parseIndex(mappedKoPos.xpath, "/body/DocFragment[");
  const bool loneFragment = docFrag < 0 && mappedKoPos.xpath.compare(0, 17, "/body/DocFragment") == 0;
  const int xpathSpine = loneFragment ? 0 : (docFrag >= 1) ? (docFrag - 1) : -1;
  if (xpathSpine >= 0 && xpathSpine < spineCount) {
    result.spineIndex = xpathSpine;
  } else {
    for (int i = 0; i < spineCount; i++) {
      if (epub->getCumulativeSpineItemSize(i) >= targetBytes) {
        result.spineIndex = i;
        break;
      }
    }
  }
  if (result.spineIndex >= spineCount) return result;

  const size_t prevCum = (result.spineIndex > 0) ? epub->getCumulativeSpineItemSize(result.spineIndex - 1) : 0;
  const size_t spineSize = epub->getCumulativeSpineItemSize(result.spineIndex) - prevCum;

  float intra = 0.0f;
  ChapterXPathResolver::Resolved resolved;
  const bool exact = xpathSpine == result.spineIndex &&
                     ChapterXPathResolver::resolve(epub, result.spineIndex, mappedKoPos.xpath, resolved);
  if (exact) {
    result.visibleTextOffset = resolved.visibleOffset;
    result.hasVisibleTextOffset = true;
    intra = resolved.totalVisible > 0
                ? static_cast<float>(resolved.visibleOffset) / static_cast<float>(resolved.totalVisible)
                : 0.0f;
    if (resolved.paragraphIndex > 0) {
      result.paragraphIndex = resolved.paragraphIndex;
      result.hasParagraphIndex = true;
    }
    if (resolved.targetIsListItem && resolved.listItemIndex > 0) {
      result.liIndex = resolved.listItemIndex;
      result.hasLiIndex = true;
    }
  } else if (spineSize > 0) {
    // Nothing verified inside this chapter: the percentage, but never out of the chapter it names.
    const size_t bytesIn = (targetBytes > prevCum) ? (targetBytes - prevCum) : 0;
    intra = std::max(0.0f, std::min(1.0f, static_cast<float>(bytesIn) / static_cast<float>(spineSize)));
    LOG_DBG("PM", "XPointer not found, chapter fraction %.3f: %s", intra, mappedKoPos.xpath.c_str());
  }

  if (result.spineIndex == currentSpineIndex && totalPagesInCurrentSpine > 0) {
    result.totalPages = totalPagesInCurrentSpine;
  } else if (currentSpineIndex >= 0 && currentSpineIndex < spineCount && totalPagesInCurrentSpine > 0) {
    const size_t pc = (currentSpineIndex > 0) ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
    const size_t cs = epub->getCumulativeSpineItemSize(currentSpineIndex) - pc;
    if (cs > 0)
      result.totalPages = std::max(
          1, static_cast<int>(totalPagesInCurrentSpine * static_cast<float>(spineSize) / static_cast<float>(cs)));
  }
  if (result.totalPages == 0) {
    // No layout to go by, say a book just downloaded. The visible offset names the page once the
    // reader has laid the chapter out; without one, a page out of many carries the fraction.
    static constexpr int kFractionPages = 1000;
    result.totalPages = result.hasVisibleTextOffset ? 1 : kFractionPages;
  }

  result.pageNumber = std::max(
      0, std::min(static_cast<int>(intra * static_cast<float>(result.totalPages - 1) + 0.5f), result.totalPages - 1));
  LOG_DBG("PM", "<- KO: %.2f%% %s -> spine=%d page=%d/%d", koPos.percentage * 100, mappedKoPos.xpath.c_str(),
          result.spineIndex, result.pageNumber, result.totalPages);
  return result;
}
