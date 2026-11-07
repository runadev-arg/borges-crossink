// Other readers' highlights in the page: where a highlight lands, what the card keeps, what a crash
// leaves, and what a book with none of them pays.

#include <atomic>
#include <cstdlib>
#include <new>
#include <random>
#include <string>
#include <vector>

#include "Inbound.h"
#include "TestSupport.h"
#include "Underlines.h"

using namespace borges;
using namespace borges_test;

namespace {
std::atomic<size_t> gAllocations{0};
}  // namespace

// Every allocation of this test binary, so a path that promises none can be held to it.
void* operator new(const size_t n) {
  gAllocations++;
  void* p = std::malloc(n ? n : 1);
  if (!p) std::abort();
  return p;
}
void* operator new(const size_t n, const std::nothrow_t&) noexcept {
  gAllocations++;
  return std::malloc(n ? n : 1);
}
void* operator new[](const size_t n) { return operator new(n); }
void* operator new[](const size_t n, const std::nothrow_t& t) noexcept { return operator new(n, t); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

namespace {

// A chapter as a walk sees it: every character with its visible offset, spaces included.
struct Chapter {
  std::vector<uint32_t> cps;
  std::vector<uint32_t> pageStarts;  // Offsets of each page's first word.
};

struct PageWord {
  std::string text;
  bool hyphen = false;
  bool pageOnly = false;
};
using PageWords = std::vector<std::vector<PageWord>>;

uint32_t decodeOne(const char*& p) {
  const auto* u = reinterpret_cast<const unsigned char*>(p);
  uint32_t cp = *u;
  int extra = 0;
  if (cp >= 0xF0) {
    cp &= 7;
    extra = 3;
  } else if (cp >= 0xE0) {
    cp &= 15;
    extra = 2;
  } else if (cp >= 0xC0) {
    cp &= 31;
    extra = 1;
  }
  for (int i = 1; i <= extra; ++i) cp = (cp << 6) | (u[i] & 0x3F);
  p += extra + 1;
  return cp;
}

// The words, laid out into pages of wordsPerPage.
void layOut(const std::vector<std::string>& words, const size_t wordsPerPage, Chapter& chapter, PageWords& pages) {
  for (size_t i = 0; i < words.size(); ++i) {
    if (i % wordsPerPage == 0) {
      chapter.pageStarts.push_back(static_cast<uint32_t>(chapter.cps.size()));
      pages.emplace_back();
    }
    pages.back().push_back({words[i], false, false});
    for (const char* p = words[i].c_str(); *p;) chapter.cps.push_back(decodeOne(p));
    chapter.cps.push_back(' ');
  }
}

struct Target {
  uint32_t at;  // Offset in the chapter of the highlight's first character.
  std::string text;
};

std::vector<UnderlineSegment> plan(const Chapter& chapter, const std::vector<Target>& wanted,
                                   uint16_t* verified = nullptr, uint16_t* refused = nullptr,
                                   const uint16_t cap = kUnderlineMaxSegments) {
  std::vector<UnderlineTarget> targets;
  std::vector<std::pair<uint32_t, uint16_t>> hits;
  for (const Target& w : wanted) {
    UnderlineTarget t;
    EXPECT_TRUE(underlineTextSignature(w.text.c_str(), t.hash, t.chars));
    hits.push_back({w.at, static_cast<uint16_t>(targets.size())});
    targets.push_back(t);
  }
  std::vector<UnderlineSegment> out(cap);
  UnderlinePlanner planner(targets.data(), static_cast<uint16_t>(targets.size()), chapter.pageStarts.data(),
                           static_cast<uint16_t>(chapter.pageStarts.size()), out.data(), cap);
  for (uint32_t at = 0; at < chapter.cps.size(); ++at) {
    for (const auto& hit : hits) {
      if (hit.first == at) planner.onHit(hit.second, at);
    }
    if (planner.onChar(chapter.cps[at], at)) break;
  }
  out.resize(planner.finish());
  if (verified) *verified = planner.verified();
  if (refused) *refused = planner.refused();
  return out;
}

// What a page underlines, as the words a person would read, joined by counted characters.
std::string underlinedOn(const std::vector<PageWord>& words, const std::vector<UnderlineSegment>& all,
                         const uint16_t page) {
  std::vector<UnderlineSegment> here;
  for (const auto& s : all) {
    if (s.page == page) here.push_back(s);
  }
  UnderlinePageMatch match(here.data(), static_cast<uint8_t>(here.size()));
  for (const auto& w : words) match.feedWord(w.text.c_str(), w.text.size(), w.hyphen, w.pageOnly);
  match.finish();
  std::string out;
  uint32_t position = 0;
  for (const auto& w : words) {
    const uint32_t chars = UnderlinePageMatch::wordChars(w.text.c_str(), w.text.size(), w.hyphen);
    uint32_t from = 0;
    uint32_t to = 0;
    if (match.spanOf(position, chars, w.pageOnly, from, to)) {
      const size_t a = UnderlinePageMatch::byteOfChar(w.text.c_str(), w.text.size(), from);
      const size_t b = UnderlinePageMatch::byteOfChar(w.text.c_str(), w.text.size(), to);
      out += w.text.substr(a, b - a);
    }
    position += chars;
  }
  return out;
}

std::string withoutSpaces(const std::string& text) {
  std::string out;
  for (const char c : text) {
    if (c != ' ' && c != '\n') out += c;
  }
  return out;
}

const std::vector<std::string> kWords = {"Las",      "palabras",    "siguen",   "por",    "el",       "corredor", "de",
                                         "la",       "biblioteca,", "hexagono", "tras",   "hexagono", "para",     "que",
                                         "cada",     "pagina",      "tenga",    "texto",  "de",       "verdad",   "y",
                                         "el",       "lector",      "tenga",    "que",    "pasarla",  "entera,",  "con",
                                         "acentos:", "año,",        "canción,", "último."};

}  // namespace

TEST(UnderlineChars, WhatADeviceSpacesDifferentlyDoesNotCount) {
  uint32_t a = 0;
  uint32_t b = 0;
  uint16_t n = 0;
  uint16_t m = 0;
  ASSERT_TRUE(underlineTextSignature("El  a\xC2\xADmor\n es\xC2\xA0 ciego", a, n));
  ASSERT_TRUE(underlineTextSignature("El amor es ciego", b, m));
  EXPECT_EQ(a, b);
  EXPECT_EQ(n, m);
  // Composed or not, and in capitals by a stylesheet.
  ASSERT_TRUE(underlineTextSignature("cancio\xCC\x81n", a, n));
  ASSERT_TRUE(underlineTextSignature("canci\xC3\xB3n", b, m));
  EXPECT_EQ(a, b);
  EXPECT_EQ(n, m);
  ASSERT_TRUE(underlineTextSignature("CANCION", b, m));
  EXPECT_EQ(a, b);
  EXPECT_FALSE(underlineTextSignature(" \n\xC2\xA0", a, n));
  ASSERT_TRUE(underlineTextSignature("otra cosa", b, m));
  EXPECT_NE(a, b);
}

TEST(UnderlinePlanner, OneHighlightOnOnePage) {
  Chapter chapter;
  PageWords pages;
  layOut(kWords, 10, chapter, pages);
  // "corredor de la" sits at word 5 of the first page.
  const uint32_t at = 3 + 1 + 8 + 1 + 6 + 1 + 3 + 1 + 2 + 1;
  uint16_t verified = 0;
  const auto segments = plan(chapter, {{at, "corredor de la"}}, &verified);
  EXPECT_EQ(verified, 1);
  ASSERT_EQ(segments.size(), 1u);
  EXPECT_EQ(segments[0].page, 0);
  EXPECT_EQ(segments[0].delta, 3 + 8 + 6 + 3 + 2);
  EXPECT_EQ(segments[0].length, 8 + 2 + 2);
  EXPECT_EQ(underlinedOn(pages[0], segments, 0), "corredordela");
}

TEST(UnderlinePlanner, AcrossPagesEachPageHasItsPart) {
  Chapter chapter;
  PageWords pages;
  layOut(kWords, 4, chapter, pages);
  const uint32_t at = chapter.pageStarts[1];
  const std::string text = "el corredor de la biblioteca, hexagono tras hexagono para que";
  uint16_t verified = 0;
  const auto segments = plan(chapter, {{at, text}}, &verified);
  EXPECT_EQ(verified, 1);
  ASSERT_EQ(segments.size(), 3u);
  std::string seen;
  for (uint16_t p = 0; p < pages.size(); ++p) seen += underlinedOn(pages[p], segments, p);
  EXPECT_EQ(seen, withoutSpaces(text));
}

TEST(UnderlinePlanner, TextThatIsNotThereIsNotDrawn) {
  Chapter chapter;
  PageWords pages;
  layOut(kWords, 10, chapter, pages);
  uint16_t verified = 0;
  uint16_t refused = 0;
  EXPECT_TRUE(plan(chapter, {{0, "Las palabras siguen por otro lado"}}, &verified, &refused).empty());
  EXPECT_EQ(verified, 0);
  EXPECT_EQ(refused, 1);
  // Runs past the end of the chapter.
  EXPECT_TRUE(
      plan(chapter, {{chapter.pageStarts.back(), "Y esto no termina nunca jamas en la vida de nadie"}}).empty());
}

TEST(UnderlinePlanner, ARefusedOneLeavesItsNeighboursParts) {
  Chapter chapter;
  PageWords pages;
  layOut(kWords, 3, chapter, pages);
  // Both span a page break, interleaved: only the one whose text is right stays.
  const uint32_t a = chapter.pageStarts[0] + 4;
  const auto segments = plan(chapter, {{a, "palabras siguen por el"}, {a + 9, "siguen por XX corredor"}});
  std::string seen;
  for (uint16_t p = 0; p < pages.size(); ++p) seen += underlinedOn(pages[p], segments, p);
  EXPECT_EQ(seen, "palabrassiguenporel");
}

TEST(UnderlinePlanner, BoundsAreRefusalsNotOverruns) {
  Chapter chapter;
  PageWords pages;
  layOut(kWords, 2, chapter, pages);
  std::string all;
  for (const auto& w : kWords) all += w + " ";
  // More segments than there is room for: the highlight goes whole.
  uint16_t verified = 0;
  uint16_t refused = 0;
  EXPECT_TRUE(plan(chapter, {{0, all}}, &verified, &refused, 2).empty());
  EXPECT_EQ(verified, 0);
  EXPECT_EQ(refused, 1);
  // More at once than the walk follows.
  std::vector<Target> many;
  for (int i = 0; i < kUnderlineActiveMax + 4; ++i) many.push_back({0, "Las palabras"});
  verified = 0;
  refused = 0;
  const auto segments = plan(chapter, many, &verified, &refused);
  EXPECT_EQ(verified, kUnderlineActiveMax);
  EXPECT_EQ(refused, 4);
  EXPECT_EQ(segments.size(), static_cast<size_t>(kUnderlineActiveMax));
}

TEST(UnderlinePlanner, StopsTheWalkWhenThereIsNothingLeftToFind) {
  Chapter chapter;
  PageWords pages;
  layOut(kWords, 10, chapter, pages);
  UnderlineTarget target;
  ASSERT_TRUE(underlineTextSignature("Las", target.hash, target.chars));
  std::vector<UnderlineSegment> out(4);
  UnderlinePlanner planner(&target, 1, chapter.pageStarts.data(), static_cast<uint16_t>(chapter.pageStarts.size()),
                           out.data(), 4);
  planner.onHit(0, 0);
  EXPECT_FALSE(planner.onChar('L', 0));
  EXPECT_FALSE(planner.onChar('a', 1));
  EXPECT_TRUE(planner.onChar('s', 2));
}

TEST(UnderlinePage, AWordCutByAHyphenCountsOnce) {
  std::vector<PageWord> words = {
      {"La", false, false}, {"biblio-", true, false}, {"teca", false, false}, {"es", false, false}};
  UnderlineSegment segment;
  segment.page = 0;
  segment.delta = 2;
  segment.length = 10;
  uint32_t hash = 0;
  uint16_t chars = 0;
  ASSERT_TRUE(underlineTextSignature("biblioteca", hash, chars));
  segment.hash = hash;
  EXPECT_EQ(underlinedOn(words, {segment}, 0), "biblioteca");
}

TEST(UnderlinePage, AWordTheReaderAddedDoesNotPushTheCountOff) {
  // A list item: "1." is the reader's and not in the book, so the book's count starts after it.
  std::vector<PageWord> words = {
      {"1.", false, true}, {"Primero", false, false}, {"2.", false, true}, {"Segundo", false, false}};
  UnderlineSegment segment;
  segment.page = 0;
  segment.delta = 7;
  segment.length = 7;
  ASSERT_TRUE([&] {
    uint16_t chars = 0;
    return underlineTextSignature("Segundo", segment.hash, chars);
  }());
  EXPECT_EQ(underlinedOn(words, {segment}, 0), "Segundo");
}

TEST(UnderlinePage, AHighlightAcrossListItemsLeavesTheMarkerOut) {
  std::vector<PageWord> words = {
      {"1.", false, true}, {"Primero", false, false}, {"2.", false, true}, {"Segundo", false, false}};
  UnderlineSegment segment;
  segment.page = 0;
  segment.delta = 0;
  segment.length = 14;
  uint16_t chars = 0;
  ASSERT_TRUE(underlineTextSignature("Primero\nSegundo", segment.hash, chars));
  EXPECT_EQ(underlinedOn(words, {segment}, 0), "PrimeroSegundo");
}

TEST(UnderlinePage, OtherWordsAtThatPlaceAreNotUnderlined) {
  std::vector<PageWord> words = {{"Otra", false, false}, {"cosa", false, false}};
  UnderlineSegment segment;
  segment.page = 0;
  segment.delta = 4;
  segment.length = 4;
  uint16_t chars = 0;
  ASSERT_TRUE(underlineTextSignature("casa", segment.hash, chars));
  EXPECT_EQ(underlinedOn(words, {segment}, 0), "");
}

TEST(UnderlinePage, PartOfAWordIsOnlyThatPart) {
  // A highlight that leaves out the comma and the opening quote.
  std::vector<PageWord> words = {
      {"\xC2\xAB"
       "amor,",
       false, false}};
  UnderlineSegment segment;
  segment.page = 0;
  segment.delta = 1;
  segment.length = 4;
  uint16_t chars = 0;
  ASSERT_TRUE(underlineTextSignature("amor", segment.hash, chars));
  EXPECT_EQ(underlinedOn(words, {segment}, 0), "amor");
}

TEST(UnderlinePage, TheReadersOwnMarkers) {
  for (const char* yes : {"1.", "12.", "-3.", "\xE2\x80\xA2"}) {
    EXPECT_TRUE(UnderlinePageMatch::looksLikeListMarker(yes, strlen(yes))) << yes;
  }
  for (const char* no : {"a.", "1", ".", "1.2", "palabra."}) {
    EXPECT_FALSE(UnderlinePageMatch::looksLikeListMarker(no, strlen(no))) << no;
  }
}

// Any highlight, on any layout: what the pages underline is its text and nothing else.
TEST(UnderlineRoundTrip, RandomHighlightsOnRandomLayouts) {
  std::mt19937 random(20261106);
  for (int round = 0; round < 300; ++round) {
    std::vector<std::string> words;
    const size_t count = 40 + random() % 400;
    for (size_t i = 0; i < count; ++i) words.push_back(kWords[random() % kWords.size()]);
    Chapter chapter;
    PageWords pages;
    layOut(words, 3 + random() % 60, chapter, pages);
    const size_t first = random() % count;
    const size_t last = std::min(count - 1, first + random() % 40);
    uint32_t at = 0;
    for (size_t i = 0; i < first; ++i) {
      for (const char* p = words[i].c_str(); *p;) {
        decodeOne(p);
        ++at;
      }
      ++at;
    }
    std::string text;
    for (size_t i = first; i <= last; ++i) text += (i > first ? " " : "") + words[i];
    uint16_t verified = 0;
    const auto segments = plan(chapter, {{at, text}}, &verified);
    ASSERT_EQ(verified, 1) << text;
    std::string seen;
    for (uint16_t p = 0; p < pages.size(); ++p) seen += underlinedOn(pages[p], segments, p);
    ASSERT_EQ(seen, withoutSpaces(text));
  }
}

// ---- on the card -------------------------------------------------------------------------------

namespace {
constexpr const char* kPlanPath = "/plan.bin";
constexpr const char* kSpinesPath = "/spines.bin";

UnderlinePlanKey keyOf(const uint32_t log = 7, const uint32_t own = 8, const uint32_t layout = 9,
                       const uint16_t pages = 30) {
  UnderlinePlanKey key;
  key.log = log;
  key.own = own;
  key.layout = layout;
  key.pages = pages;
  return key;
}

std::vector<UnderlineSegment> someSegments(const uint16_t count) {
  std::vector<UnderlineSegment> segments(count);
  for (uint16_t i = 0; i < count; ++i) {
    segments[i].page = static_cast<uint16_t>(i / 3);
    segments[i].delta = static_cast<uint16_t>(i * 7);
    segments[i].length = static_cast<uint16_t>(i + 1);
    segments[i].hash = i * 2654435761u;
  }
  return segments;
}
}  // namespace

TEST(UnderlinePlanFile, ComesBackAsItWent) {
  MemFs fs;
  const auto segments = someSegments(kUnderlineMaxSegments);
  ASSERT_TRUE(saveUnderlinePlan(fs, kPlanPath, keyOf(), segments.data(), kUnderlineMaxSegments));
  uint16_t count = 0;
  uint8_t bits[kUnderlinePageBytes] = {0};
  ASSERT_TRUE(loadUnderlinePlan(fs, kPlanPath, keyOf(), count, bits));
  EXPECT_EQ(count, kUnderlineMaxSegments);
  EXPECT_NE(bits[0], 0);
  UnderlineSegment out[kUnderlinePageMax];
  uint32_t covers = 0;
  const uint8_t found = readUnderlineWindow(fs, kPlanPath, count, 5, out, kUnderlinePageMax, covers);
  ASSERT_EQ(found, kUnderlinePageMax);
  EXPECT_EQ(out[0].page, 5);
  EXPECT_EQ(out[0].delta, 15 * 7);
  EXPECT_EQ(out[2].length, 18);
  EXPECT_EQ(out[2].hash, 17u * 2654435761u);
  // Three a page: 16 of them answer for five pages whole, and the sixth is not vouched for.
  EXPECT_EQ(covers, 10u);
  EXPECT_EQ(readUnderlineWindow(fs, kPlanPath, count, 1000, out, kUnderlinePageMax, covers), 0);
  EXPECT_EQ(covers, UINT32_MAX) << "nothing to the end of the chapter: no more reads";
  // The last of them: everything left fits.
  const uint8_t tail = readUnderlineWindow(fs, kPlanPath, count, count / 3 - 1, out, kUnderlinePageMax, covers);
  EXPECT_LE(tail, kUnderlinePageMax);
  EXPECT_EQ(covers, UINT32_MAX);
}

TEST(UnderlinePlanFile, AChangeInWhatItDependsOnMakesItStale) {
  MemFs fs;
  const auto segments = someSegments(5);
  ASSERT_TRUE(saveUnderlinePlan(fs, kPlanPath, keyOf(), segments.data(), 5));
  uint16_t count = 0;
  uint8_t bits[kUnderlinePageBytes] = {0};
  EXPECT_FALSE(loadUnderlinePlan(fs, kPlanPath, keyOf(70), count, bits)) << "another state of the log";
  EXPECT_FALSE(loadUnderlinePlan(fs, kPlanPath, keyOf(7, 80), count, bits)) << "this reader's clippings moved";
  EXPECT_FALSE(loadUnderlinePlan(fs, kPlanPath, keyOf(7, 8, 90), count, bits)) << "another font or orientation";
  EXPECT_FALSE(loadUnderlinePlan(fs, kPlanPath, keyOf(7, 8, 9, 31), count, bits)) << "another page count";
  EXPECT_TRUE(loadUnderlinePlan(fs, kPlanPath, keyOf(), count, bits));
}

TEST(UnderlinePlanFile, ABitFlipIsNotAPlan) {
  MemFs fs;
  const auto segments = someSegments(20);
  ASSERT_TRUE(saveUnderlinePlan(fs, kPlanPath, keyOf(), segments.data(), 20));
  std::string good;
  {
    File f(fs, kPlanPath, IFs::Mode::Read);
    ASSERT_TRUE(static_cast<bool>(f));
    good.resize(static_cast<size_t>(f.size()));
    ASSERT_TRUE(f.readExact(good.data(), good.size()));
  }
  uint16_t count = 0;
  uint8_t bits[kUnderlinePageBytes] = {0};
  for (size_t i = 0; i < good.size(); ++i) {
    std::string bad = good;
    bad[i] = static_cast<char>(bad[i] ^ 0x10);
    {
      File f(fs, kPlanPath, IFs::Mode::WriteTrunc);
      ASSERT_TRUE(f.writeAll(bad.data(), bad.size()) && f.flush());
    }
    EXPECT_FALSE(loadUnderlinePlan(fs, kPlanPath, keyOf(), count, bits)) << "byte " << i;
  }
}

// A cut at every write, torn every way: what is on the card is then the plan or nothing, and a
// missing one is walked again.
TEST(UnderlinePlanFile, ACrashLeavesThePlanOrNothing) {
  const auto segments = someSegments(30);
  for (const TornMode mode : {TornMode::Keep, TornMode::Drop, TornMode::Half}) {
    for (uint64_t kill = 1;; ++kill) {
      MemFs fs;
      fs.setTornMode(mode);
      fs.killAtOp(kill);
      const bool saved = saveUnderlinePlan(fs, kPlanPath, keyOf(), segments.data(), 30);
      const bool died = fs.dead();
      fs.reboot();
      uint16_t count = 0;
      uint8_t bits[kUnderlinePageBytes] = {0};
      const bool loaded = loadUnderlinePlan(fs, kPlanPath, keyOf(), count, bits);
      if (loaded) {
        EXPECT_EQ(count, 30);
        UnderlineSegment out[kUnderlinePageMax];
        uint32_t covers = 0;
        EXPECT_EQ(readUnderlineWindow(fs, kPlanPath, count, 2, out, kUnderlinePageMax, covers), kUnderlinePageMax);
      }
      if (saved) EXPECT_TRUE(loaded);
      // And the next one replaces whatever was there.
      ASSERT_TRUE(saveUnderlinePlan(fs, kPlanPath, keyOf(), segments.data(), 30));
      ASSERT_TRUE(loadUnderlinePlan(fs, kPlanPath, keyOf(), count, bits));
      if (!died) break;
    }
  }
}

TEST(UnderlineSpines, ACrashLeavesTheListOrNothing) {
  uint8_t bits[kUnderlineSpineBytes] = {0};
  bits[3] = 0x41;
  for (uint64_t kill = 1;; ++kill) {
    MemFs fs;
    fs.killAtOp(kill);
    const bool saved = saveUnderlineSpines(fs, kSpinesPath, 5, bits);
    const bool died = fs.dead();
    fs.reboot();
    uint8_t back[kUnderlineSpineBytes] = {0};
    const bool loaded = loadUnderlineSpines(fs, kSpinesPath, 5, back);
    if (loaded) EXPECT_EQ(memcmp(bits, back, sizeof(bits)), 0);
    if (saved) EXPECT_TRUE(loaded);
    if (!died) break;
  }
  MemFs fs;
  ASSERT_TRUE(saveUnderlineSpines(fs, kSpinesPath, 5, bits));
  uint8_t back[kUnderlineSpineBytes] = {0};
  EXPECT_FALSE(loadUnderlineSpines(fs, kSpinesPath, 6, back)) << "the log moved on";
  EXPECT_TRUE(underlineSpineBit(bits, 3 * 8));
  EXPECT_TRUE(underlineSpineBit(bits, 3 * 8 + 6));
  EXPECT_FALSE(underlineSpineBit(bits, 3 * 8 + 1));
}

// ---- from the log ------------------------------------------------------------------------------

namespace {

BookId bookOf(const char c) {
  BookId id;
  uint8_t raw[16];
  memset(raw, c, sizeof(raw));
  id.setRaw(raw);
  return id;
}

InboundHighlight highlightAt(const int n, const int fragment, const char* text,
                             const char* pointerTail = "/body/p/text().0") {
  InboundHighlight h;
  h.syncId[0] = static_cast<uint8_t>(n);
  h.syncId[1] = static_cast<uint8_t>(n >> 8);
  h.serverSeq = static_cast<uint64_t>(n) + 1;
  h.revision = 1;
  h.passage = passageHash(text);
  snprintf(h.xpointer, sizeof(h.xpointer), "/body/DocFragment[%d]%s", fragment + 1, pointerTail);
  snprintf(h.text, sizeof(h.text), "%s", text);
  return h;
}

bool fakeKey(const char* xpointer, uint32_t& key, uint32_t& offset) {
  key = static_cast<uint32_t>(strlen(xpointer));
  offset = 0;
  return strstr(xpointer, "nokey") == nullptr;
}

class CountingFs final : public MemFs {
 public:
  Handle open(const char* path, const Mode mode) override {
    ++opens;
    return MemFs::open(path, mode);
  }
  int32_t read(const Handle h, void* dst, const size_t len) override {
    ++reads;
    return MemFs::read(h, dst, len);
  }
  bool exists(const char* path) override {
    ++lookups;
    return MemFs::exists(path);
  }
  int opens = 0;
  int reads = 0;
  int lookups = 0;
};

}  // namespace

TEST(UnderlineLog, OneChapterAtATime) {
  MemFs fs;
  const BookId book = bookOf(1);
  {
    InboundStore store(fs);
    ASSERT_TRUE(store.apply(book, 3, highlightAt(1, 0, "uno")));
    ASSERT_TRUE(store.apply(book, 3, highlightAt(2, 4, "dos")));
    ASSERT_TRUE(store.apply(book, 3, highlightAt(3, 4, "tres")));
    ASSERT_TRUE(store.apply(book, 3, highlightAt(4, 4, "   ")));
    ASSERT_TRUE(store.apply(book, 3, highlightAt(5, 4, "sin clave", "/body/nokey/text().0")));
    ASSERT_TRUE(store.finish());
  }
  uint8_t spines[kUnderlineSpineBytes] = {0};
  ASSERT_TRUE(collectUnderlineSpines(fs, book, 3, spines));
  EXPECT_TRUE(underlineSpineBit(spines, 0));
  EXPECT_TRUE(underlineSpineBit(spines, 4));
  EXPECT_FALSE(underlineSpineBit(spines, 1));

  UnderlineTarget targets[8];
  uint16_t count = 0;
  ASSERT_TRUE(collectUnderlineTargets(fs, book, 3, 4, nullptr, fakeKey, targets, 8, count));
  EXPECT_EQ(count, 2) << "two with text and a key, in this chapter";
  ASSERT_TRUE(collectUnderlineTargets(fs, book, 9, 4, nullptr, fakeKey, targets, 8, count));
  EXPECT_EQ(count, 0) << "another account's log reads as empty";
}

TEST(UnderlineLog, WhatThisReaderHasStaysItsOwn) {
  MemFs fs;
  const BookId book = bookOf(2);
  {
    InboundStore store(fs);
    ASSERT_TRUE(store.apply(book, 1, highlightAt(1, 2, "mio")));
    ASSERT_TRUE(store.apply(book, 1, highlightAt(2, 2, "ajeno")));
    ASSERT_TRUE(store.apply(book, 1, highlightAt(3, 2, "ajeno")));
    ASSERT_TRUE(store.finish());
  }
  class Own final : public IOwnPassages {
   public:
    bool isOwn(const uint64_t passage, int32_t) override { return passage == passageHash("mio"); }
  } own;
  UnderlineTarget targets[8];
  uint16_t count = 0;
  ASSERT_TRUE(collectUnderlineTargets(fs, book, 1, 2, &own, fakeKey, targets, 8, count));
  EXPECT_EQ(count, 1) << "its own is left out, and two devices with one passage are one";
}

TEST(UnderlineLog, TheLogSaysWhenItChanged) {
  MemFs fs;
  const BookId book = bookOf(3);
  InboundStore store(fs);
  ASSERT_TRUE(store.apply(book, 1, highlightAt(1, 0, "uno")));
  ASSERT_TRUE(store.finish());
  uint32_t first = 0;
  ASSERT_TRUE(InboundList::identity(fs, book, 1, first));
  uint32_t again = 0;
  ASSERT_TRUE(InboundList::identity(fs, book, 1, again));
  EXPECT_EQ(first, again);
  ASSERT_TRUE(store.apply(book, 1, highlightAt(2, 0, "dos")));
  ASSERT_TRUE(store.finish());
  uint32_t appended = 0;
  ASSERT_TRUE(InboundList::identity(fs, book, 1, appended));
  EXPECT_NE(appended, first);
  InboundHighlight gone = highlightAt(1, 0, "uno");
  gone.serverSeq = 10;
  gone.deleted = true;
  ASSERT_TRUE(store.apply(book, 1, gone));
  ASSERT_TRUE(store.finish());
  uint32_t compacted = 0;
  ASSERT_TRUE(InboundList::identity(fs, book, 1, compacted));
  EXPECT_NE(compacted, appended);
  EXPECT_FALSE(InboundList::identity(fs, book, 2, compacted)) << "another account's log has no identity here";
  EXPECT_FALSE(InboundList::identity(fs, bookOf(9), 1, compacted)) << "a book with no log";
}

// The whole of what a book with no highlights pays, on the path a reader takes when it opens.
TEST(UnderlineCost, ACardWithNoneIsTwoLookups) {
  CountingFs fs;
  const size_t before = gAllocations.load();
  EXPECT_FALSE(InboundList::anyOnCard(fs));
  EXPECT_EQ(gAllocations.load() - before, 0u);
  EXPECT_EQ(fs.lookups, 2);
  EXPECT_EQ(fs.opens, 0);
  EXPECT_EQ(fs.reads, 0);
}

TEST(UnderlineCost, ABookWithNoneOnACardWithSomeIsTwoLookupsMore) {
  CountingFs fs;
  {
    InboundStore store(fs);
    ASSERT_TRUE(store.apply(bookOf(4), 1, highlightAt(1, 0, "de otro libro")));
    ASSERT_TRUE(store.finish());
  }
  fs.lookups = 0;
  fs.opens = 0;
  fs.reads = 0;
  const size_t before = gAllocations.load();
  EXPECT_TRUE(InboundList::anyOnCard(fs));
  EXPECT_FALSE(InboundList::present(fs, bookOf(5)));
  EXPECT_EQ(gAllocations.load() - before, 0u);
  EXPECT_LE(fs.lookups, 4);
  EXPECT_EQ(fs.opens, 0);
  EXPECT_EQ(fs.reads, 0);
}

// What a page with a highlight does between loading it and showing it: no allocation at all.
TEST(UnderlineCost, MatchingAPageAllocatesNothing) {
  Chapter chapter;
  PageWords pages;
  layOut(kWords, 32, chapter, pages);
  const auto segments = plan(chapter, {{4, "palabras siguen por el corredor"}});
  ASSERT_EQ(segments.size(), 1u);
  const size_t before = gAllocations.load();
  UnderlinePageMatch match(segments.data(), 1);
  for (const auto& w : pages[0]) match.feedWord(w.text.c_str(), w.text.size(), false, false);
  match.finish();
  EXPECT_EQ(match.acceptedCount(), 1);
  EXPECT_EQ(gAllocations.load() - before, 0u);
  EXPECT_LE(sizeof(UnderlineBook), 400u) << "what stays in RAM while reading a book that has highlights";
}
