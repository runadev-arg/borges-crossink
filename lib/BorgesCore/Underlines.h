#pragma once

// Other readers' highlights underlined in the page. Everything here runs without a screen or a book:
// the glue (src/borges/BorgesUnderlines) brings the chapter's text and the page's words.
//
//   <book cache>/ulb.bin         Which spine items have a highlight, for one state of the log
//   <book cache>/ulp<spine>.bin  Where they fall on the pages of one layout of that spine item
//
// A segment says "on this page, from the Nth counted character, this many, and these are the ones":
// the hash lets the page refuse a place that holds other words, which is what makes a miscount cost
// a missing underline and never a wrong one. Counted characters are what is left of a text after
// taking out what a device may space or join differently (see underlineCounts()).
//
// Both files are one write, checked whole by a CRC when read, so a tear reads as "not there" and the
// walk happens again. Nothing else depends on them: losing either costs time, not data.
//
// Memory. Resident while reading, and only in a book that has highlights: sizeof(UnderlineBook), about
// 370 B. While a chapter is planned, with the framebuffer lent to inflate: 32 B per highlight in the
// chapter, 12 B per segment, 4 B per page, and the walk's own. While a page is matched and drawn, only
// the stack (under 1 KB) and the handle of the one file read for a page that has segments.

#include <cstddef>
#include <cstdint>

#include "Inbound.h"
#include "Ports.h"
#include "Types.h"

namespace borges {

constexpr uint16_t kUnderlineMaxTargets = kInboundMaxRecords;
constexpr uint16_t kUnderlineMaxSegments = 512;
constexpr uint16_t kUnderlineMaxPages = 2048;
constexpr uint8_t kUnderlineActiveMax = 8;
constexpr uint8_t kUnderlinePageMax = 16;
constexpr size_t kUnderlineSpineBytes = 64;
constexpr size_t kUnderlinePageBytes = 32;

// Whether a codepoint counts. Out: white space of any width, soft hyphens, combining marks. A reader
// composes what it can, so "e" and a mark in the source is one character on its page.
bool underlineCounts(uint32_t cp);
// What is compared of one that counts: the base of a composed letter, and ASCII without case, since a
// stylesheet can show a chapter in capitals. Costs a table scan on non-ASCII: only inside a highlight.
uint32_t underlineFold(uint32_t cp);
constexpr uint32_t kUnderlineHashBasis = 2166136261u;
inline uint32_t underlineHashStep(const uint32_t hash, const uint32_t folded) {
  uint32_t h = hash;
  for (uint32_t c = folded; c != 0; c >>= 8) h = (h ^ (c & 0xFF)) * 16777619u;
  return h;
}

// A highlight's text as the chapter's walk will see it. False: nothing in it counts.
bool underlineTextSignature(const char* text, uint32_t& hash, uint16_t& chars);

struct UnderlineTarget {
  uint32_t key = 0;     // The node the xpointer names, as the resolver hashes it.
  uint32_t offset = 0;  // Its offset in that node.
  uint32_t hash = 0;
  uint32_t passage = 0;
  uint16_t chars = 0;
};

struct UnderlineSegment {
  uint16_t page = 0;
  uint16_t delta = 0;   // Counted characters on the page before it.
  uint16_t length = 0;  // Counted characters in it.
  uint16_t owner = 0;   // Whose it is, while planning. Not saved.
  uint32_t hash = 0;
};

// Which pointers of the log land in a spine item. The xpointer is only known to the resolver.
using UnderlinePointerKey = bool (*)(const char* xpointer, uint32_t& key, uint32_t& offset);

// Fills targets, sorted by (key, offset), and says how many. What this device highlighted itself stays
// out, as in the list, and the same place from two other devices counts once, as the newest.
bool collectUnderlineTargets(IFs& fs, const BookId& book, uint16_t epoch, int32_t spine, IOwnPassages* own,
                             UnderlinePointerKey keyOf, UnderlineTarget* targets, uint16_t cap, uint16_t& count);

// Which spine items of the book have one, as bits (spine % 512). A collision only costs a look.
bool collectUnderlineSpines(IFs& fs, const BookId& book, uint16_t epoch, uint8_t bits[kUnderlineSpineBytes]);
inline bool underlineSpineBit(const uint8_t bits[kUnderlineSpineBytes], const int32_t spine) {
  const auto at = static_cast<uint32_t>(spine) % (kUnderlineSpineBytes * 8);
  return (bits[at / 8] & (1u << (at % 8))) != 0;
}

// Walks a spine item's text once and says where each highlight falls. Fed by the chapter's walk.
class UnderlinePlanner {
 public:
  // pageStarts: the visible offset of each page's first word. Both arrays outlive the planner.
  UnderlinePlanner(const UnderlineTarget* targets, uint16_t targetCount, const uint32_t* pageStarts, uint16_t pageCount,
                   UnderlineSegment* out, uint16_t outCap);

  void onHit(uint16_t target, uint32_t at);
  // True when no highlight is left to find, and the walk may stop.
  bool onChar(uint32_t cp, uint32_t at);
  // Drops what did not end, and sorts by page. Returns how many segments there are.
  uint16_t finish();

  uint16_t verified() const { return verified_; }
  uint16_t refused() const { return refused_; }

 private:
  struct Active {
    uint32_t hash;
    uint32_t segmentHash;
    uint16_t target;
    uint16_t seen;
    uint16_t segmentStart;
    uint16_t segmentLength;
    uint16_t firstOut;
  };

  void advancePage(uint32_t at);
  void closeSegment(Active& active);
  void forget(const Active& active);
  void drop(uint8_t index);

  const UnderlineTarget* targets_;
  uint16_t targetCount_;
  const uint32_t* pageStarts_;
  uint16_t pageCount_;
  UnderlineSegment* out_;
  uint16_t outCap_;
  uint16_t outCount_ = 0;
  uint16_t page_ = 0;
  uint32_t counted_ = 0;
  uint16_t hits_ = 0;
  uint16_t verified_ = 0;
  uint16_t refused_ = 0;
  uint8_t activeCount_ = 0;
  Active active_[kUnderlineActiveMax];
};

// What makes a saved plan still true.
struct UnderlinePlanKey {
  uint32_t log = 0;     // InboundList::identity().
  uint32_t own = 0;     // This device's clippings in the spine item.
  uint32_t layout = 0;  // The section's render spec.
  uint16_t pages = 0;
};

bool saveUnderlineSpines(IFs& fs, const char* path, uint32_t logIdentity, const uint8_t bits[kUnderlineSpineBytes]);
bool loadUnderlineSpines(IFs& fs, const char* path, uint32_t logIdentity, uint8_t bits[kUnderlineSpineBytes]);

bool saveUnderlinePlan(IFs& fs, const char* path, const UnderlinePlanKey& key, const UnderlineSegment* segments,
                       uint16_t count);
// True when the file is whole and for this key. No allocation. pageBits: (page % 256) that have one.
bool loadUnderlinePlan(IFs& fs, const char* path, const UnderlinePlanKey& key, uint16_t& count,
                       uint8_t pageBits[kUnderlinePageBytes]);
// The segments from a page on, at most cap (kUnderlinePageMax at most), so that the next pages need no
// file. coversBefore: the first page these do not fully answer for. A page with more than cap keeps cap.
uint8_t readUnderlineWindow(IFs& fs, const char* path, uint16_t total, uint16_t page, UnderlineSegment* out,
                            uint8_t cap, uint32_t& coversBefore);

// What the reader keeps for an open book that has highlights. Fixed size, in one allocation.
struct UnderlineBook {
  BookId book;
  uint16_t epoch = 0;
  uint32_t logIdentity = 0;
  uint8_t spineBits[kUnderlineSpineBytes] = {0};
  int32_t planSpine = -1;
  uint32_t planLayout = 0;
  uint16_t planSegments = 0;
  bool planTried = false;
  uint8_t pageBits[kUnderlinePageBytes] = {0};
  // The next few pages' segments, so a chapter full of them does not read a file on every turn.
  uint32_t windowFirst = 1;
  uint32_t windowEnd = 0;  // Nothing in the window: windowFirst is past it.
  uint8_t windowCount = 0;
  UnderlineSegment window[kUnderlinePageMax];
};

// Decides, for one page, which segments are really there and where. Fed the page's words in reading
// order, without allocating. Fed twice is not needed: the words are then walked again to draw, asking
// spanOf() about each.
class UnderlinePageMatch {
 public:
  UnderlinePageMatch(const UnderlineSegment* segments, uint8_t count);

  // pageOnly: this word is the reader's own and not in the book, as "1." in front of a list item.
  void feedWord(const char* text, size_t length, bool insertedHyphen, bool pageOnly);
  void finish();

  uint8_t acceptedCount() const { return accepted_; }

  // What part of a word to mark, in counted characters of that word. wordFirstChar is where the word
  // starts in the whole walk. False: none of it. A word that is the reader's own is marked only by a
  // highlight that had it in its text, and never by one that was only found by leaving it out.
  bool spanOf(uint32_t wordFirstChar, uint32_t wordChars, bool pageOnly, uint32_t& charFrom, uint32_t& charTo) const;

  // Counted characters in a word, without an inserted hyphen.
  static uint32_t wordChars(const char* text, size_t length, bool insertedHyphen);
  // The byte at which a counted character of a word starts.
  static size_t byteOfChar(const char* text, size_t length, uint32_t charIndex);
  // A word the reader adds in front of a list item: a bullet or a number and a dot.
  static bool looksLikeListMarker(const char* text, size_t length);

 private:
  struct Run {
    uint32_t hash = kUnderlineHashBasis;
    uint32_t rawStart = 0;
    uint32_t rawEnd = 0;
    uint16_t seen = 0;
  };
  struct State {
    Run all;     // Counting every word on the page.
    Run strict;  // Without the reader's own words.
    uint32_t from = 0;
    uint32_t to = 0;
    bool ok = false;
    bool leftOutTheReaders = false;
  };

  const UnderlineSegment* segments_;
  uint8_t count_;
  uint8_t accepted_ = 0;
  uint32_t all_ = 0;
  uint32_t strict_ = 0;
  State states_[kUnderlinePageMax];
};

}  // namespace borges
