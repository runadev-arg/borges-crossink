#pragma once

#include <Epub.h>

#include <cstdint>
#include <memory>
#include <string>

/**
 * Converts between the firmware's visible text offset and crengine (KOReader) xpointers.
 *
 * It streams the spine item through expat and reproduces, with bounded memory and no recursion,
 * the part of crengine's DOM that xpointers can see: which text nodes exist, how each one is
 * collapsed, and how siblings are numbered. Internal boxing elements (autoBoxing, floatBox...)
 * do not appear: since DOM version 20200223 crengine's xpointers skip them. Everything here is
 * held to KOReader's real engine by test/borges_xpointer.
 */
class ChapterXPathResolver {
 public:
  struct Resolved {
    uint32_t visibleOffset = 0;   // Zero-based, in the firmware's coordinate
    uint32_t totalVisible = 0;    // Length of the spine item in that coordinate
    uint16_t paragraphIndex = 0;  // <p> elements opened up to the position
    uint16_t listItemIndex = 0;   // <li> elements opened up to the position
    bool targetIsListItem = false;
    bool exact = true;  // False: the element is there, but not that text node. Nearest position.
  };

  /**
   * Find the position an xpointer names. False when the document does not have the element it
   * names, which is when crengine's createXPointer() refuses it too.
   */
  static bool resolve(const std::shared_ptr<Epub>& epub, int spineIndex, const std::string& xpointer, Resolved& out);

  /** The start of a chapter: "/body/DocFragment[N]/body". */
  static std::string bodyXPath(const std::shared_ptr<Epub>& epub, int spineIndex);

  /** The Nth (1-based) paragraph of a spine item, as an element xpointer. Empty when missing. */
  static std::string findXPathForParagraph(const std::shared_ptr<Epub>& epub, int spineIndex, uint16_t paragraphIndex);

  /** The Nth (1-based) list item of a spine item, as an element xpointer. Empty when missing. */
  static std::string findXPathForListItem(const std::shared_ptr<Epub>& epub, int spineIndex, uint16_t listItemIndex);

  /** The character at a fraction of the spine item. Empty when it has no text or cannot be read. */
  static std::string findXPathForProgress(const std::shared_ptr<Epub>& epub, int spineIndex, float intraSpineProgress,
                                          uint8_t minimumPathDepth = 0);

  /**
   * The xpointer of the first non-space character at or after a visible offset; past the last
   * one, of the last one. crengine accepts the result and lands on that same character. Empty
   * when the offset is beyond the spine item or it cannot be read.
   *
   * minimumPathDepth keeps split-book anchors inside the mapped children.
   */
  static std::string findXPathForVisibleTextOffset(const std::shared_ptr<Epub>& epub, int spineIndex,
                                                   uint32_t visibleTextOffset, uint8_t minimumPathDepth = 0);
};
