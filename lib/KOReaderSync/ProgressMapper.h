#pragma once
#include <Epub.h>

#include <memory>
#include <optional>
#include <string>

#include "KOReaderSyncClient.h"

/**
 * CrossPoint position representation.
 */
struct CrossPointPosition {
  int spineIndex;                  // Current spine item (chapter) index
  int pageNumber;                  // Current page within the spine item
  int totalPages;                  // Total pages in the current spine item
  uint16_t paragraphIndex = 0;     // 1-based synthetic paragraph index from XPath p[N]
  bool hasParagraphIndex = false;  // True when paragraphIndex was resolved from XPath
  uint16_t liIndex = 0;            // Running <li> count at the matched XPath element
  bool hasLiIndex = false;         // True when target element is <li> and liIndex was resolved
  char xpathAnchorId[64] = {};     // First <a id> captured inside the matched XPath element
  uint32_t visibleTextOffset = 0;  // Zero-based visible codepoint offset in the spine item
  bool hasVisibleTextOffset = false;
  bool valid = true;  // False when an exact requested coordinate mapping is unavailable
};

/**
 * KOReader position representation.
 */
struct KOReaderPosition {
  std::string xpath;  // XPath-like progress string
  float percentage;   // Progress percentage (0.0 to 1.0)
  bool valid = true;  // False when an exact requested coordinate mapping is unavailable
};

enum class PositionCoordinateSpace : uint8_t {
  CurrentDocument,
  SourceDocument,
};

/**
 * Maps between CrossPoint and KOReader position formats.
 *
 * CrossPoint tracks position as (spineIndex, pageNumber).
 * KOReader uses XPath-like strings + percentage.
 *
 * The mapping goes through the visible text offset, the coordinate the section
 * cache keeps per page, and ChapterXPathResolver, which is held to KOReader's
 * real engine by test/borges_xpointer.
 */
class ProgressMapper {
 public:
  /**
   * Convert CrossPoint position to KOReader format.
   *
   * @param epub The EPUB book
   * @param pos CrossPoint position
   * @return KOReader position
   */
  static KOReaderPosition toKOReader(
      const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos,
      PositionCoordinateSpace coordinateSpace = PositionCoordinateSpace::CurrentDocument);

  /**
   * Convert KOReader position to CrossPoint format.
   *
   * The exact position is visibleTextOffset. pageNumber is an estimate until the
   * section cache turns that offset into a page, and is all there is when the
   * xpointer is not in this book: then the percentage is used, inside the chapter
   * the xpointer names.
   *
   * @param epub The EPUB book
   * @param koPos KOReader position
   * @param currentSpineIndex Index of the currently open spine item (for density estimation)
   * @param totalPagesInCurrentSpine Total pages in the current spine item (for density estimation)
   * @return CrossPoint position
   */
  static CrossPointPosition toCrossPoint(
      const std::shared_ptr<Epub>& epub, const KOReaderPosition& koPos, int currentSpineIndex = -1,
      int totalPagesInCurrentSpine = 0,
      PositionCoordinateSpace coordinateSpace = PositionCoordinateSpace::CurrentDocument);

  /**
   * Convert a rich CrossPoint position (downloaded from a crosspoint-sync
   * server) directly to a CrossPoint position, without XPath approximation.
   * When the local layout matches the uploader's (same spine page count) the
   * page transfers losslessly; otherwise the paragraph LUT or the intra-spine
   * page fraction is used.
   *
   * @return The position, or std::nullopt when the rich position cannot be
   *         applied (spine out of range, no section cache) and the caller
   *         should fall back to toCrossPoint().
   */
  static std::optional<CrossPointPosition> fromRichPosition(const std::shared_ptr<Epub>& epub,
                                                            const KOReaderRichPosition& rich, GfxRenderer& renderer);
};
