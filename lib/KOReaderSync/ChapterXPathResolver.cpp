#include "ChapterXPathResolver.h"

#include <Epub/htmlEntities.h>
#include <Epub/parsers/VisibleOffsetRules.h>
#include <Logging.h>
#include <Print.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>
#include <strings.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

namespace {

// ---------------------------------------------------------------------------------------------
// crengine's element table (crengine/include/fb2def.h). An element it does not know is inline
// and allows text. epub.css changes none of these for a document opened today.
// ---------------------------------------------------------------------------------------------
constexpr char kBlockText[] =
    " address article aside blockquote body book-name book-title button canvas caption center city custom-info "
    "datalist date dd details dialog dir div dl dt email fieldset figcaption figure footer form frame frameset h1 h2 "
    "h3 h4 h5 h6 header hgroup history home-page hr id iframe isbn keywords lang legend li listing main map marquee "
    "menu multicol nav nickname noembed noframes noscript ol optgroup output p plaintext pre program-used publisher "
    "search section select sequence src-lang src-ocr src-url subtitle summary td template text-author textarea th "
    "title ul v version video xmp year ";
constexpr char kBlockNoText[] =
    " annotation applet area audio author col colgroup coverpage document-info empty-line epigraph head html input "
    "keygen param part poem publish-info source src-title-info stanza table tbody tfoot thead title-info tr track ";
constexpr char kHiddenText[] = " binary genre script style stylesheet ";
constexpr char kHiddenNoText[] = " base basefont bgsound description link meta ";
constexpr char kInlineNoText[] = " embed image img object svg ";
constexpr char kPre[] = " listing plaintext pre style textarea xmp ";
// Text inside these is not stored as written, or only partly: no exact offset can be named.
constexpr char kOpaque[] = " math svg ";
constexpr char kSvgText[] = " desc metadata style text textPath title tspan ";

constexpr size_t kMaxTagLen = 24;
constexpr int kTextSplitSize = 8192;  // TEXT_SPLIT_SIZE in crengine/src/lvxml.cpp

bool inWordList(const char* list, const char* name, const size_t len) {
  for (const char* p = list; (p = strstr(p, name)) != nullptr; p += len) {
    if (p[-1] == ' ' && p[len] == ' ') return true;
  }
  return false;
}

enum class Display : uint8_t { Inline, Block, None };

// Reads one property out of a style="..." attribute. The value, lowercased, goes in `value`.
bool inlineStyleValue(const char* style, const char* prop, char* value, const size_t valueSize) {
  const size_t propLen = strlen(prop);
  for (const char* p = style; *p;) {
    while (*p == ' ' || *p == ';' || *p == '\t' || *p == '\n') p++;
    const char* colon = strchr(p, ':');
    if (!colon) return false;
    const char* nameEnd = colon;
    while (nameEnd > p && (nameEnd[-1] == ' ' || nameEnd[-1] == '\t')) nameEnd--;
    const char* v = colon + 1;
    while (*v == ' ' || *v == '\t') v++;
    if (static_cast<size_t>(nameEnd - p) == propLen && strncasecmp(p, prop, propLen) == 0) {
      size_t n = 0;
      while (v[n] && v[n] != ';' && v[n] != ' ' && v[n] != '!' && n + 1 < valueSize) {
        value[n] = (v[n] >= 'A' && v[n] <= 'Z') ? static_cast<char>(v[n] + 32) : v[n];
        n++;
      }
      value[n] = '\0';
      return true;
    }
    const char* semi = strchr(v, ';');
    if (!semi) return false;
    p = semi + 1;
  }
  return false;
}

// A lone DocFragment has no index, as ldomXPointer::toStringV2() writes it.
std::string bodyPointer(const int spineIndex, const int spineCount) {
  if (spineCount <= 1) return "/body/DocFragment/body";
  return "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
}

constexpr uint16_t kWindows1252[32] = {0x20AC, 0x81,   0x201A, 0x192,  0x201E, 0x2026, 0x2020, 0x2021,
                                       0x2C6,  0x2030, 0x160,  0x2039, 0x152,  0x8D,   0x17D,  0x8F,
                                       0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                       0x2DC,  0x2122, 0x161,  0x203A, 0x153,  0x9D,   0x17E,  0x178};

uint32_t hashName(const char* s, const size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) h = (h ^ static_cast<uint8_t>(s[i])) * 16777619u;
  return h;
}

// Expat reports namespaced names as written; crengine keeps only the local part, lowercased.
size_t localName(const char* raw, char* out, const size_t outSize) {
  const char* colon = strrchr(raw, ':');
  const char* s = colon ? colon + 1 : raw;
  size_t n = 0;
  for (; s[n] && n + 1 < outSize; n++) {
    out[n] = (s[n] >= 'A' && s[n] <= 'Z') ? static_cast<char>(s[n] + 32) : s[n];
  }
  out[n] = '\0';
  return s[n] ? 0 : n;
}

// A parsed "/body/DocFragment[N]/body/a[2]/b/text()[3].45". Boxing elements only appear in
// xpointers from DOM versions before 20200223, and are skipped: their children count in the
// unboxed parent.
struct ParsedPointer {
  static constexpr int kMaxSteps = 32;
  struct Step {
    uint32_t hash;
    uint16_t index;
  };
  Step steps[kMaxSteps];
  int stepCount = 0;
  int textIndex = 0;  // 0: names an element, not a text node
  int offset = -1;    // -1: no ".N" suffix
  bool targetIsListItem = false;

  bool parse(const std::string& xpointer) {
    static constexpr char kFragment[] = "/body/DocFragment";
    if (xpointer.compare(0, sizeof(kFragment) - 1, kFragment) != 0) return false;
    size_t pos = sizeof(kFragment) - 1;
    if (pos < xpointer.size() && xpointer[pos] == '[') {
      pos = xpointer.find(']', pos);
      if (pos == std::string::npos) return false;
      pos++;
    }
    if (pos == xpointer.size()) return true;
    if (xpointer[pos] == '.') return parseOffset(xpointer, pos);
    static constexpr char kBody[] = "/body";
    if (xpointer.compare(pos, sizeof(kBody) - 1, kBody) != 0) return false;
    pos += sizeof(kBody) - 1;
    if (pos < xpointer.size() && xpointer[pos] == '[') {
      const size_t close = xpointer.find(']', pos);
      if (close == std::string::npos) return false;
      pos = close + 1;
    }
    while (pos < xpointer.size()) {
      if (xpointer[pos] == '.') return parseOffset(xpointer, pos);
      if (xpointer[pos] != '/') return false;
      pos++;
      size_t end = pos;
      while (end < xpointer.size() && xpointer[end] != '/' && xpointer[end] != '.' && xpointer[end] != '[') end++;
      const size_t nameLen = end - pos;
      if (nameLen == 0) return false;
      int index = 1;
      size_t after = end;
      if (after < xpointer.size() && xpointer[after] == '[') {
        index = 0;
        after++;
        while (after < xpointer.size() && xpointer[after] >= '0' && xpointer[after] <= '9') {
          if (index > 100000) return false;
          index = index * 10 + (xpointer[after++] - '0');
        }
        if (after >= xpointer.size() || xpointer[after] != ']' || index == 0) return false;
        after++;
      }
      if (nameLen == 6 && xpointer.compare(pos, 6, "text()") == 0) {
        textIndex = index;
        pos = after;
        if (pos < xpointer.size() && xpointer[pos] != '.') return false;
        if (pos < xpointer.size()) return parseOffset(xpointer, pos);
        return true;
      }
      const bool isBox =
          xpointer.compare(pos, nameLen, "autoBoxing") == 0 || xpointer.compare(pos, nameLen, "tabularBox") == 0 ||
          xpointer.compare(pos, nameLen, "rubyBox") == 0 || xpointer.compare(pos, nameLen, "mathBox") == 0 ||
          xpointer.compare(pos, nameLen, "floatBox") == 0 || xpointer.compare(pos, nameLen, "inlineBox") == 0;
      if (!isBox) {
        if (stepCount >= kMaxSteps) return false;
        steps[stepCount].hash = hashName(xpointer.data() + pos, nameLen);
        steps[stepCount].index = static_cast<uint16_t>(index);
        stepCount++;
        targetIsListItem = nameLen == 2 && xpointer.compare(pos, 2, "li") == 0;
      }
      pos = after;
    }
    return true;
  }

 private:
  bool parseOffset(const std::string& xpointer, size_t pos) {
    pos++;
    if (pos >= xpointer.size()) return false;
    int value = 0;
    for (; pos < xpointer.size(); pos++) {
      if (xpointer[pos] < '0' || xpointer[pos] > '9' || value > 100000000) return false;
      value = value * 10 + (xpointer[pos] - '0');
    }
    offset = value;
    return true;
  }
};

// ---------------------------------------------------------------------------------------------
// The walker: expat events in, crengine's view of the document out.
// ---------------------------------------------------------------------------------------------
class CreWalker final : public Print {
 public:
  enum class Mode : uint8_t {
    Count,           // Totals only
    EmitOffset,      // Offset to xpointer
    EmitElement,     // Nth <p> or <li> to xpointer
    ResolvePointer,  // Xpointer to offset
    LocateText,      // A clipping's text to the offsets of its ends
    CollectText      // What crengine reads between two offsets
  };

  CreWalker(const Mode mode, const int spineIndex, const int spineCount)
      : mode_(mode), spineIndex_(spineIndex), spineCount_(spineCount) {
    parser_ = XML_ParserCreate(nullptr);
    if (!parser_) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }
    XML_SetUserData(parser_, this);
    XML_SetElementHandler(parser_, &CreWalker::startThunk, &CreWalker::endThunk);
    XML_SetCharacterDataHandler(parser_, &CreWalker::charsThunk);
    XML_SetCommentHandler(parser_, &CreWalker::commentThunk);
    XML_SetProcessingInstructionHandler(parser_, &CreWalker::piThunk);
    XML_SetCdataSectionHandler(parser_, &CreWalker::cdataThunk, &CreWalker::cdataThunk);
    // The same entity handling as ChapterHtmlSlimParser: the offset has to count alike.
    XML_SetDefaultHandlerExpand(parser_, &CreWalker::defaultThunk);
  }

  ~CreWalker() override { destroyXmlParser(parser_); }

  void setTargetOffset(const uint32_t target, const uint8_t minDepth) {
    targetOffset_ = target;
    minDepth_ = minDepth;
  }
  void setCssParser(CssParser* css) { css_ = (css && !css->empty()) ? css : nullptr; }
  void setTargetElement(const char* tag, const int ordinal) {
    targetTagHash_ = hashName(tag, strlen(tag));
    targetOrdinal_ = ordinal;
  }
  void setPointer(const ParsedPointer* pointer) { pointer_ = pointer; }
  void setLocate(const char* text, const uint32_t anchor, const uint16_t expectedWord) {
    locateText_ = text;
    locateAnchored_ = anchor != UINT32_MAX;
    targetOffset_ = locateAnchored_ ? anchor : 0;
    expectedWord_ = expectedWord;
  }
  void setCollect(const uint32_t first, const uint32_t last, std::string* out) {
    targetOffset_ = first;
    collectLast_ = last;
    collectOut_ = out;
  }
  bool located() const { return locateMatches_ > 0 && (locateAnchored_ || locateMatches_ == 1); }
  uint32_t locatedFirst() const { return locateBestFirst_; }
  uint32_t locatedLast() const { return locateBestLast_; }

  bool ok() const { return parser_ != nullptr && parseOk_; }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser_ || !parseOk_ || stopped_) return size;
    if (XML_Parse(parser_, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser_);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk_ = false;
      }
    }
    return size;
  }

  void finish() {
    if (!parser_ || !parseOk_ || stopped_) return;
    if (XML_Parse(parser_, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser_)));
      parseOk_ = false;
    }
  }

  uint32_t visibleChars() const { return visible_; }
  uint32_t lastEligibleOffset() const { return lastEligible_; }
  bool hasEligible() const { return hasEligible_; }
  bool sawBody() const { return bodySeen_; }
  bool hasMatch() const { return found_; }
  uint32_t matchOffset() const { return matchOffset_; }
  bool matchExact() const { return exact_; }
  uint16_t paragraphs() const { return paragraphAtMatch_; }
  uint16_t listItems() const { return listItemAtMatch_; }

  std::string xpointer() const {
    if (!found_) return "";
    std::string out = bodyPointer(spineIndex_, spineCount_);
    const char* name = snapNames_;
    for (int i = 0; i < snapCount_; i++) {
      out += '/';
      out.append(name, snap_[i].nameLen);
      name += snap_[i].nameLen;
      // Only a complete read proves there is no second one. crengine accepts "[1]" either way.
      if (snap_[i].index > 1 || snap_[i].hasLater || !parseOk_) {
        out += '[';
        out += std::to_string(snap_[i].index);
        out += ']';
      }
    }
    if (snapTextIndex_ > 0) {
      out += "/text()";
      if (snapTextIndex_ > 1 || snapTextTotal_ > snapTextIndex_ || !parseOk_) {
        out += '[';
        out += std::to_string(snapTextIndex_);
        out += ']';
      }
      out += '.';
      out += std::to_string(snapTextOffset_);
    }
    return out;
  }

 private:
  static constexpr int kMaxDepth = 40;
  static constexpr int kMaxCounters = 64;
  static constexpr int kNamePool = 280;

  struct Frame {
    uint16_t nameOffset;
    uint16_t index;  // 1-based among same-named siblings
    uint16_t texts;  // Text nodes of this element that survive so far
    uint16_t kids;   // Children of any kind that survive so far
    uint8_t nameLen;
    uint8_t counterBase;  // First sibling counter that belongs to this element's children
    bool block : 1;
    bool allowText : 1;
    bool pre : 1;
    bool stripNewline : 1;
    bool hasChild : 1;
    bool runLeading : 1;  // Nothing but empty text since the last non-inline child
    bool afterBlock : 1;  // That child was a real block
    bool lastEmptyText : 1;
    bool hasBlock : 1;
    bool hidden : 1;
    bool opaque : 1;
    bool svg : 1;
    bool countersFull : 1;
    bool inTable : 1;
    bool absentFromClip : 1;
  };

  struct Counter {
    uint32_t hash;
    uint16_t count;
  };

  static constexpr int kMaxAttempts = 12;
  static constexpr uint32_t kLocateSlackWords = 64;
  struct Attempt {
    uint32_t position;
    uint32_t first;
    uint32_t last;
    uint32_t startWord;
    bool afterBase;
    bool markSeen;
  };

  static constexpr int kCssMemoSize = 16;
  static constexpr uint8_t kCssHasDisplay = 1, kCssHasWhiteSpace = 2, kCssPre = 4, kCssDisplayShift = 3;
  struct CssMemo {
    uint32_t key = 0;
    uint8_t found = 0;
  };

  struct SnapStep {
    uint32_t hash;
    uint16_t index;
    uint8_t nameLen;
    bool hasLater;
  };

  // ---- expat thunks ------------------------------------------------------------------------
  static void XMLCALL startThunk(void* d, const XML_Char* name, const XML_Char** atts) {
    static_cast<CreWalker*>(d)->onStart(name, atts);
  }
  static void XMLCALL endThunk(void* d, const XML_Char* name) { static_cast<CreWalker*>(d)->onEnd(name); }
  static void XMLCALL charsThunk(void* d, const XML_Char* s, int len) { static_cast<CreWalker*>(d)->onChars(s, len); }
  static void XMLCALL commentThunk(void* d, const XML_Char*) { static_cast<CreWalker*>(d)->endChunk(); }
  static void XMLCALL cdataThunk(void* d) { static_cast<CreWalker*>(d)->endChunk(); }
  static void XMLCALL piThunk(void* d, const XML_Char*, const XML_Char*) { static_cast<CreWalker*>(d)->onPi(); }
  static void XMLCALL defaultThunk(void* d, const XML_Char* s, int len) {
    static_cast<CreWalker*>(d)->onDefault(s, len);
  }

  // ---- helpers ------------------------------------------------------------------------------
  void stop() {
    stopped_ = true;
    XML_StopParser(parser_, XML_FALSE);
  }

  void styleOf(const char* name, const size_t nameLen, const XML_Char** atts, const bool inheritedPre, Display& display,
               bool& pre) {
    display = Display::Inline;
    if (inWordList(kBlockText, name, nameLen) || inWordList(kBlockNoText, name, nameLen)) {
      display = Display::Block;
    } else if (inWordList(kHiddenText, name, nameLen) || inWordList(kHiddenNoText, name, nameLen)) {
      display = Display::None;
    }
    pre = inheritedPre || inWordList(kPre, name, nameLen);

    const char* style = nullptr;
    const char* classes = nullptr;
    for (int i = 0; atts && atts[i]; i += 2) {
      if (strcmp(atts[i], "style") == 0) style = atts[i + 1];
      if (strcmp(atts[i], "class") == 0) classes = atts[i + 1];
    }
    if (css_) {
      const uint8_t found = styleFromCss(name, nameLen, classes);
      if (found & kCssHasDisplay) {
        display = static_cast<Display>((found >> kCssDisplayShift) & 3);
      }
      if (found & kCssHasWhiteSpace) pre = (found & kCssPre) != 0;
    }
    if (!style) return;
    char value[16];
    if (inlineStyleValue(style, "display", value, sizeof(value))) {
      if (strcmp(value, "none") == 0) {
        display = Display::None;
      } else if (strcmp(value, "inline") == 0 || strcmp(value, "inline-block") == 0 ||
                 strcmp(value, "inline-table") == 0) {
        display = Display::Inline;
      } else if (value[0] && strcmp(value, "inherit") != 0) {
        display = Display::Block;
      }
    }
    if (inlineStyleValue(style, "white-space", value, sizeof(value))) {
      pre = strncmp(value, "pre", 3) == 0 || strcmp(value, "break-spaces") == 0;
    }
  }

  // setNodeStyle() in lvrend.cpp: whatever a publisher sets, a <br> stays inline unless hidden.
  static void applyForcedDisplay(const char* name, const size_t nameLen, Display& display) {
    if (nameLen == 2 && name[0] == 'b' && name[1] == 'r' && display != Display::None) display = Display::Inline;
  }

  // Classes repeat all through a chapter, and a look-up may seek the SD card.
  uint8_t styleFromCss(const char* name, const size_t nameLen, const char* classes) {
    uint32_t key = hashName(name, nameLen);
    if (classes) key = (key * 31) ^ hashName(classes, strlen(classes));
    key |= 1;  // Zero marks a free slot
    CssMemo& memo = cssMemo_[(key >> 1) % kCssMemoSize];
    if (memo.key == key) return memo.found;
    const CssStyle resolved = css_->resolveStyle(name, classes ? classes : "");
    uint8_t found = 0;
    if (resolved.hasDisplay()) {
      const Display d = resolved.display == CssDisplay::None     ? Display::None
                        : resolved.display == CssDisplay::Inline ? Display::Inline
                                                                 : Display::Block;
      found |= kCssHasDisplay | static_cast<uint8_t>(static_cast<uint8_t>(d) << kCssDisplayShift);
    }
    if (resolved.defined.whiteSpace) {
      found |= kCssHasWhiteSpace;
      if (resolved.defined.whiteSpacePreserved) found |= kCssPre;
    }
    memo.key = key;
    memo.found = found;
    return found;
  }

  // ---- clippings -----------------------------------------------------------------------------
  // What a clipping and the source may differ by: the reader and crengine each break and space
  // the text their own way, and comparableText() in the plugin ignores a soft hyphen.
  static bool ignoredInClip(const uint32_t cp) {
    return isSpaceCp(cp) || (cp >= 0x80 && cp <= 0xA0) || cp == 0xAD || cp == 0xFEFF ||
           (cp >= 0x2000 && cp <= 0x200B) || cp == 0x202F || cp == 0x205F || cp == 0x3000;
  }

  // The next character of the clipping that counts, and where the one after it starts.
  uint32_t clipCodepointAt(uint32_t position, uint32_t& next) const {
    const auto* base = reinterpret_cast<const unsigned char*>(locateText_);
    for (;;) {
      // A stand-in for an image the reader could not show is not in the source.
      static constexpr char kImage[] = "[Image:";
      if (strncmp(locateText_ + position, kImage, sizeof(kImage) - 1) == 0) {
        const char* close = strchr(locateText_ + position, ']');
        position = close ? static_cast<uint32_t>(close + 1 - locateText_) : static_cast<uint32_t>(strlen(locateText_));
        continue;
      }
      const unsigned char* ptr = base + position;
      if (*ptr == 0) {
        next = position;
        return 0;
      }
      const uint32_t cp = utf8NextCodepoint(&ptr);
      position = static_cast<uint32_t>(ptr - base);
      if (ignoredInClip(cp)) continue;
      next = position;
      return cp;
    }
  }

  // The reader's own marker of a list item, "1." or a bullet, is in the clipping and not the source.
  uint32_t afterListMarker(const uint32_t position) const {
    const char* p = locateText_ + position;
    while (*p == ' ' || *p == '\n') p++;
    if (strncmp(p, "\xE2\x80\xA2", 3) == 0) return static_cast<uint32_t>(p + 3 - locateText_);
    const char* digits = p;
    while (*p >= '0' && *p <= '9') p++;
    if (p != digits && *p == '.') return static_cast<uint32_t>(p + 1 - locateText_);
    return position;
  }

  // One source character against what the clipping expects at a position. The reader composes
  // what it can, so the clipping's "e acute" may be "e" and a combining mark in the source.
  bool advance(Attempt& attempt, const uint32_t cp) const {
    uint32_t next = 0;
    uint32_t wanted = clipCodepointAt(attempt.position, next);
    for (int tries = 0; tries < 2; tries++) {
      if (wanted == cp) {
        attempt.position = next;
        attempt.afterBase = false;
        return true;
      }
      if (wanted != 0 && utf8DecomposedBase(wanted) == cp) {
        attempt.position = next;
        attempt.afterBase = true;
        attempt.markSeen = false;
        return true;
      }
      const uint32_t skipped = afterListMarker(attempt.position);
      if (skipped == attempt.position) return false;
      attempt.position = skipped;
      wanted = clipCodepointAt(attempt.position, next);
    }
    return false;
  }

  // The base alone is not that letter: without its mark, this is other text.
  bool finished(const Attempt& attempt) const {
    if (attempt.afterBase && !attempt.markSeen) return false;
    return clipFinished(attempt.position);
  }

  bool clipFinished(const uint32_t position) const {
    uint32_t after = 0;
    return clipCodepointAt(position, after) == 0 || clipCodepointAt(afterListMarker(position), after) == 0;
  }

  uint32_t distanceToExpected(const uint32_t word) const {
    return word > expectedWord_ ? word - expectedWord_ : expectedWord_ - word;
  }

  void startAttempt(const uint32_t position, const uint32_t cp, const uint32_t at) {
    Attempt attempt{position, at, at, wordsSeen_ > 0 ? wordsSeen_ - 1 : 0, false, false};
    if (!advance(attempt, cp)) return;
    if (finished(attempt)) {
      finishAttempt(attempt);
      return;
    }
    if (attemptCount_ < kMaxAttempts) {
      attempts_[attemptCount_++] = attempt;
      return;
    }
    // Text that repeats can fill the slots. Keep the ones nearest where it should start.
    int farthest = 0;
    for (int i = 1; i < attemptCount_; i++) {
      if (distanceToExpected(attempts_[i].startWord) > distanceToExpected(attempts_[farthest].startWord)) farthest = i;
    }
    if (distanceToExpected(attempt.startWord) < distanceToExpected(attempts_[farthest].startWord)) {
      attempts_[farthest] = attempt;
    }
  }

  void feedLocate(const uint32_t cp, const uint32_t at) {
    const bool mark = cp >= 0x300 && cp <= 0x36F;
    // A mark on the clipping's last character belongs to the range.
    if (mark && locateMatches_ > 0 && at == locateBestLast_ + 1) locateBestLast_ = at;
    int kept = 0;
    for (int i = 0; i < attemptCount_; i++) {
      Attempt attempt = attempts_[i];
      if (mark && attempt.afterBase) {
        attempt.last = at;
        attempt.markSeen = true;
        if (finished(attempt)) {
          finishAttempt(attempt);
          continue;
        }
        attempts_[kept++] = attempt;
        continue;
      }
      if (attempt.afterBase && !attempt.markSeen) continue;
      if (!advance(attempt, cp)) continue;
      attempt.last = at;
      if (finished(attempt)) {
        finishAttempt(attempt);
        continue;
      }
      attempts_[kept++] = attempt;
    }
    attemptCount_ = static_cast<uint8_t>(kept);
    startAttempt(0, cp, at);
    if (locateAnchored_) {
      const uint32_t limit = expectedWord_ + kLocateSlackWords;
      // Past where it could start, with nothing under way: the answer will not change.
      if (wordsSeen_ > limit && attemptCount_ == 0) {
        tracking_ = false;
        stop();
      }
    } else if (locateMatches_ > 1) {
      tracking_ = false;
      stop();
    }
  }

  void finishAttempt(const Attempt& attempt) {
    const uint32_t distance = distanceToExpected(attempt.startWord);
    if (locateMatches_ == 0 || distance < locateBestDistance_) {
      locateBestDistance_ = distance;
      locateBestFirst_ = attempt.first;
      locateBestLast_ = attempt.last;
    }
    locateMatches_++;
  }

  void collectChar(const uint32_t cp, const int stored, const Frame& frame) {
    if (cp == 0xAD || stored == 0) return;  // ldomXRange::getRangeText() removes soft hyphens
    const uint32_t mapped = (cp >= 0x80 && cp < 0xA0) ? kWindows1252[cp - 0x80] : cp;
    if (isSpaceCp(mapped)) {
      collectOut_->push_back(' ');
      return;
    }
    if (!collectInChunk_ && frame.runLeading && frame.afterBlock) collectNewBlock_ = true;
    collectInChunk_ = true;
    if (collectNewBlock_ && !collectOut_->empty()) collectOut_->push_back('\n');
    collectNewBlock_ = false;
    char utf8[4];
    int n = 0;
    if (mapped < 0x80) {
      utf8[n++] = static_cast<char>(mapped);
    } else if (mapped < 0x800) {
      utf8[n++] = static_cast<char>(0xC0 | (mapped >> 6));
      utf8[n++] = static_cast<char>(0x80 | (mapped & 0x3F));
    } else if (mapped < 0x10000) {
      utf8[n++] = static_cast<char>(0xE0 | (mapped >> 12));
      utf8[n++] = static_cast<char>(0x80 | ((mapped >> 6) & 0x3F));
      utf8[n++] = static_cast<char>(0x80 | (mapped & 0x3F));
    } else {
      utf8[n++] = static_cast<char>(0xF0 | (mapped >> 18));
      utf8[n++] = static_cast<char>(0x80 | ((mapped >> 12) & 0x3F));
      utf8[n++] = static_cast<char>(0x80 | ((mapped >> 6) & 0x3F));
      utf8[n++] = static_cast<char>(0x80 | (mapped & 0x3F));
    }
    collectOut_->append(utf8, n);
  }

  static bool hasToken(const char* value, const char* token) {
    const size_t len = strlen(token);
    for (const char* p = value; (p = strstr(p, token)) != nullptr; p += len) {
      const bool startsWord = p == value || p[-1] == ' ';
      const bool endsWord = p[len] == 0 || p[len] == ' ';
      if (startsWord && endsWord) return true;
    }
    return false;
  }

  // The checks ChapterHtmlSlimParser makes before it skips an element.
  static bool leftOutByReader(const XML_Char** atts) {
    for (int i = 0; atts && atts[i]; i += 2) {
      if (strcmp(atts[i], "hidden") == 0) return true;
      if (strcmp(atts[i], "role") == 0 && hasToken(atts[i + 1], "doc-pagebreak")) return true;
      if (strcmp(atts[i], "epub:type") == 0 && hasToken(atts[i + 1], "pagebreak")) return true;
    }
    return false;
  }

  // ---- element events ----------------------------------------------------------------------
  void onStart(const XML_Char* rawName, const XML_Char** atts) {
    if (excludedNesting_ > 0 || isVisibleOffsetExcludedTag(rawName)) excludedNesting_++;
    if (!tracking_) return;

    char name[kMaxTagLen];
    const size_t nameLen = localName(rawName, name, sizeof(name));

    if (!inBody_) {
      if (!bodySeen_ && nameLen == 4 && strcmp(name, "body") == 0) {
        inBody_ = true;
        bodySeen_ = true;
        pushBody(atts);
      }
      return;
    }
    endChunk();

    if (overflowNesting_ > 0) {
      overflowNesting_++;
      return;
    }
    if (depth_ + 1 >= kMaxDepth || usedName_ + nameLen > kNamePool || nameLen == 0) {
      // Too deep to follow (crengine has no limit, this has to). Nothing in here is eligible.
      overflowNesting_ = 1;
      return;
    }

    if (nameLen == 1 && name[0] == 'p') paragraphCount_++;
    if (nameLen == 2 && name[0] == 'l' && name[1] == 'i') listItemCount_++;

    Frame& parent = frames_[depth_];
    Display display;
    bool pre;
    styleOf(name, nameLen, atts, parent.pre, display, pre);
    applyForcedDisplay(name, nameLen, display);
    if (mode_ == Mode::CollectText) {
      const bool isBr = nameLen == 2 && name[0] == 'b' && name[1] == 'r';
      if (!parent.hidden && (isBr || display == Display::Block ||
                             (display == Display::Inline && parent.runLeading && parent.afterBlock))) {
        collectNewBlock_ = true;
      }
    }
    if (!parent.hidden) applyChildToParent(parent, display);
    parent.hasChild = true;
    parent.kids++;
    if (tentative_) confirmTentative();

    const uint32_t hash = hashName(name, nameLen);
    const uint16_t index = nextSiblingIndex(parent, hash);
    const bool unknownIndex = parent.countersFull;

    depth_++;
    Frame& frame = frames_[depth_];
    memset(&frame, 0, sizeof(frame));
    frame.nameOffset = usedName_;
    frame.nameLen = static_cast<uint8_t>(nameLen);
    memcpy(names_ + usedName_, name, nameLen);
    usedName_ = static_cast<uint16_t>(usedName_ + nameLen);
    frame.index = index;
    frame.counterBase = usedCounters_;
    frame.block = display == Display::Block;
    frame.hidden = parent.hidden || display == Display::None;
    frame.svg = parent.svg || (nameLen == 3 && strcmp(name, "svg") == 0);
    frame.opaque = parent.opaque || unknownIndex || inWordList(kOpaque, name, nameLen);
    frame.allowText = !inWordList(kBlockNoText, name, nameLen) && !inWordList(kHiddenNoText, name, nameLen) &&
                      !inWordList(kInlineNoText, name, nameLen);
    frame.pre = pre;
    if (frame.svg) {
      frame.allowText = inWordList(kSvgText, rawName, strlen(rawName));
      frame.pre = frame.allowText;
    }
    frame.stripNewline = (nameLen == 3 && strcmp(name, "pre") == 0) || (nameLen == 8 && strcmp(name, "textarea") == 0);
    frame.runLeading = true;
    frame.inTable = parent.inTable || (nameLen == 5 && strcmp(name, "table") == 0);
    // Text the reader leaves out and crengine does not: never in a clipping, always in its range.
    frame.absentFromClip = parent.absentFromClip || (parent.inTable && nameLen == 5 && strcmp(name, "table") == 0) ||
                           leftOutByReader(atts);

    noteLaterSibling(hash);
    if (found_) return;

    switch (mode_) {
      case Mode::EmitElement:
        if (hash == targetTagHash_ && ++elementCount_ == targetOrdinal_) {
          snapshot(depth_);
          found_ = true;
          paragraphAtMatch_ = paragraphCount_;
          listItemAtMatch_ = listItemCount_;
          liveDepth_ = depth_;
          armNeeds();
        }
        break;
      case Mode::ResolvePointer:
        if (matched_ == depth_ - 1 && depth_ <= pointer_->stepCount && pointer_->steps[depth_ - 1].hash == hash &&
            pointer_->steps[depth_ - 1].index == index) {
          matched_ = depth_;
          armPointerIfComplete();
        }
        break;
      default:
        break;
    }
  }

  void pushBody(const XML_Char** atts) {
    depth_ = 0;
    Frame& frame = frames_[0];
    memset(&frame, 0, sizeof(frame));
    frame.index = 1;
    frame.block = true;
    frame.allowText = true;
    frame.runLeading = true;
    Display display;
    bool pre;
    styleOf("body", 4, atts, false, display, pre);
    frame.pre = pre;
    frame.hidden = display == Display::None;
    if (mode_ == Mode::ResolvePointer) armPointerIfComplete();
  }

  // What a new child does to the text nodes its parent has so far (see autoboxChildren() and
  // initNodeRendMethod() in lvtinydom.cpp): an element that is not inline ends a run of inlines,
  // and the empty text at the edges of a run is removed.
  void applyChildToParent(Frame& parent, const Display child) {
    switch (child) {
      case Display::Block:
        if (parent.lastEmptyText && parent.texts > 0) {
          parent.texts--;
          parent.kids--;
          dropTentative();
        }
        parent.hasBlock = true;
        parent.afterBlock = true;
        parent.runLeading = true;
        break;
      case Display::None:
        if (parent.hasBlock) {
          if (parent.lastEmptyText && parent.texts > 0) {
            parent.texts--;
            parent.kids--;
          }
          parent.afterBlock = true;
        }
        parent.runLeading = true;
        break;
      case Display::Inline:
        parent.runLeading = false;
        break;
    }
    parent.lastEmptyText = false;
  }

  uint16_t nextSiblingIndex(Frame& parent, const uint32_t hash) {
    const uint8_t limit = usedCounters_;
    for (uint8_t i = parent.counterBase; i < limit; i++) {
      if (counters_[i].hash == hash) return ++counters_[i].count;
    }
    if (usedCounters_ >= kMaxCounters) {
      parent.countersFull = true;
      return 1;
    }
    counters_[usedCounters_] = {hash, 1};
    usedCounters_++;
    return 1;
  }

  void onEnd(const XML_Char*) {
    if (excludedNesting_ > 0) excludedNesting_--;
    if (!tracking_ || !inBody_) return;
    endChunk();
    if (overflowNesting_ > 0) {
      overflowNesting_--;
      return;
    }

    Frame& frame = frames_[depth_];
    if (!frame.hidden && frame.hasBlock && frame.lastEmptyText && frame.texts > 0) {
      frame.texts--;
      dropTentative();
    }
    if (tentative_) confirmTentative();

    if (found_) {
      if (depth_ == snapCount_ && liveDepth_ == depth_ && snapTextIndex_ > 0) {
        snapTextTotal_ = frame.texts;
        if (needText_) {
          needText_ = false;
          settleNeeds();
        }
      }
      if (depth_ <= liveDepth_) liveDepth_ = depth_ - 1;
    } else if (mode_ == Mode::ResolvePointer && depth_ == matched_ && depth_ > 0) {
      matched_ = depth_ - 1;
      if (elementWaiting_) {
        // Fewer children than the point names: what follows the element.
        elementWaiting_ = false;
        elementArmed_ = true;
      } else if (pointerArmed_ && !elementArmed_) {
        // The element closed without the text node the pointer names: crengine's DOM and this
        // one disagree about it. The last text of that element is the nearest there is.
        exact_ = false;
        matchOffset_ = nearOffset_;
        matchNow();
      }
    }

    if (depth_ == 0) {
      inBody_ = false;
      if (found_) {
        if (mode_ != Mode::ResolvePointer) stop();
      } else if (mode_ == Mode::ResolvePointer && pointerArmed_) {
        // An element pointer with nothing but markup after it: the end of the spine item. A text
        // pointer directly in <body> that was not there: the last text of the body.
        const bool element = pointer_->textIndex == 0;
        exact_ = element;
        matchOffset_ = element ? visible_ : nearOffset_;
        matchNow();
      }
      return;
    }
    usedCounters_ = frame.counterBase;
    usedName_ = frame.nameOffset;
    depth_--;
    if (elementWaiting_ && depth_ == matched_) armWhenChildrenPassed();
  }

  void onPi() {
    if (!tracking_ || !inBody_ || overflowNesting_ > 0) return;
    endChunk();
    // crengine's parser makes an (empty, inline) element out of a processing instruction.
    Frame& parent = frames_[depth_];
    if (!parent.hidden) applyChildToParent(parent, Display::Inline);
    parent.hasChild = true;
  }

  // ---- text ---------------------------------------------------------------------------------
  void onDefault(const XML_Char* s, const int len) {
    // Same as ChapterHtmlSlimParser::defaultHandlerExpand: an entity expat could not expand.
    if (len < 3 || s[0] != '&' || s[len - 1] != ';') return;
    const char* value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (value) {
      feed(value, static_cast<int>(strlen(value)), true, len);
    } else {
      // crengine knows far more names than this table. Counted as written, stored as one.
      feedUnknownEntity(s, len);
    }
  }

  void onChars(const XML_Char* s, const int len) {
    if (len <= 0) return;
    const int consumed = XML_GetCurrentByteCount(parser_);
    const bool entity = consumed > len && consumed >= 4 && len <= 4;
    if (entity) {
      feed(s, len, true, consumed);
    } else {
      // A CRLF arrives as one "\n", and crengine's limit counts it as the two it was.
      feed(s, len, false, consumed > len ? consumed - len : 0);
    }
  }

  void feedUnknownEntity(const char* s, const int len) {
    if (excludedNesting_ == 0) visible_ += static_cast<uint32_t>(len);
    if (!tracking_ || !inBody_ || overflowNesting_ > 0) return;
    const uint32_t at = excludedNesting_ == 0 ? visible_ - static_cast<uint32_t>(len) : visible_;
    storeChar('?', at, false, len);
  }

  void feed(const char* s, const int len, const bool entity, const int rawExtra) {
    const auto* ptr = reinterpret_cast<const unsigned char*>(s);
    const auto* const end = ptr + len;
    bool first = true;
    while (ptr < end) {
      const uint32_t cp = utf8NextCodepoint(&ptr);
      const uint32_t at = visible_;
      if (excludedNesting_ == 0) visible_++;
      if (!tracking_ || !inBody_ || overflowNesting_ > 0) continue;
      const int raw = entity ? (first ? rawExtra : 0) : 1 + (first ? rawExtra : 0);
      first = false;
      storeChar(cp, at, !entity && isSpaceCp(cp), raw);
    }
  }

  static bool isSpaceCp(const uint32_t cp) { return cp == ' ' || cp == '\n' || cp == '\t' || cp == '\r'; }

  // One decoded character, as lvxml.cpp PreProcessXmlString() and ExpandTabs() treat it.
  void storeChar(const uint32_t cp, const uint32_t at, const bool literalSpace, const int raw) {
    Frame& frame = frames_[depth_];
    if (!frame.allowText) return;
    if (!chunkOpen_) openChunk();

    int stored = 1;
    if (frame.stripNewline) {
      frame.stripNewline = false;
      if (cp == '\n' && !frame.hasChild) stored = 0;
    }
    if (stored == 1) {
      if (frame.pre) {
        if (cp == '\t') {
          stored = 8 - (chunkColumn_ & 7);
          chunkColumn_ += stored;
        } else if (cp == '\n' || cp == '\r') {
          chunkColumn_ = 1;  // ExpandTabs() zeroes it, then counts the newline itself
        } else {
          chunkColumn_++;
        }
      } else if (literalSpace) {
        if (chunkSpaces_ > 0) stored = 0;
        chunkSpaces_++;
      } else {
        chunkSpaces_ = 0;
      }
    }

    if (!isSpaceCp(cp)) chunkHasContent_ = true;
    onStoredChar(cp, at, stored);
    chunkStored_ += stored;
    chunkRaw_ += raw;
    if (raw > 0 && literalSpace && (cp == ' ' || cp == '\n' || cp == '\r')) {
      chunkSplitRaw_ = chunkRaw_;
      chunkSplitStored_ = chunkStored_;
      chunkSplitVisible_ = visible_;
    }
    if (chunkRaw_ >= kTextSplitSize) splitChunk();
  }

  void openChunk() {
    chunkOpen_ = true;
    collectInChunk_ = false;
    chunkHasContent_ = false;
    chunkStored_ = 0;
    chunkRaw_ = 0;
    chunkSpaces_ = 0;
    chunkColumn_ = 0;
    chunkSplitRaw_ = 0;
    chunkSplitStored_ = 0;
    chunkExtraNodes_ = 0;
  }

  // TEXT_SPLIT_SIZE source characters without a '<': crengine gives what it has up to the last
  // space to a text node, and the rest starts the next one.
  void splitChunk() {
    Frame& frame = frames_[depth_];
    const int keepStored = chunkSplitRaw_ > 0 ? chunkSplitStored_ : chunkStored_;
    const int keepRaw = chunkSplitRaw_ > 0 ? chunkSplitRaw_ : chunkRaw_;
    const uint32_t restVisible = chunkSplitRaw_ > 0 ? chunkSplitVisible_ : visible_;

    if (emitRecordOpen_) {
      if (emitOffset_ < keepStored) {
        commitEmit(frame);
        emitRecordOpen_ = false;
      } else {
        emitOffset_ -= keepStored;
      }
    }
    if (mode_ == Mode::ResolvePointer && !found_ && chunkOwnsPointer()) {
      const int wanted = pointer_->offset < 0 ? 0 : pointer_->offset;
      const int node = frame.texts + 1;
      if (node == pointer_->textIndex && wanted < keepStored) {
        matchOffset_ = recordVisible_;
        matchNow();
      } else if (node == pointer_->textIndex && wanted == keepStored) {
        matchOffset_ = restVisible;
        matchNow();
      } else if (node + 1 == pointer_->textIndex && wanted < chunkStored_ - keepStored) {
        // Already read, and the rest of a split has no spaces to collapse.
        matchOffset_ = restVisible + static_cast<uint32_t>(wanted);
        matchNow();
      }
      recordOpen_ = false;
      nearOffset_ = restVisible;
    }

    frame.texts++;
    frame.kids++;
    frame.hasChild = true;
    frame.runLeading = false;
    frame.lastEmptyText = false;
    chunkStored_ -= keepStored;
    chunkRaw_ -= keepRaw;
    chunkSplitRaw_ = 0;
    chunkSplitStored_ = 0;
    chunkSpaces_ = 0;
    chunkColumn_ = 0;
    chunkHasContent_ = chunkStored_ > 0;
    chunkExtraNodes_++;
  }

  // A text node ends at any markup. It may be dropped: see ldomElementWriter::onText() and the
  // removal of empty text in autoboxChildren().
  void endChunk() {
    if (!chunkOpen_) return;
    chunkOpen_ = false;
    Frame& frame = frames_[depth_];
    const bool empty = !chunkHasContent_ && chunkExtraNodes_ == 0;

    bool survives = true;
    if (empty && !frame.pre) {
      if (frame.block && !frame.hasChild) {
        survives = false;
      } else if (!frame.hidden && frame.runLeading && frame.afterBlock) {
        survives = false;
      }
    }

    if (mode_ == Mode::ResolvePointer && !found_) {
      if (tentative_) confirmTentative();
      if (!found_ && chunkOwnsPointer() && survives) {
        if (frame.texts + 1 == pointer_->textIndex) {
          const int wanted = pointer_->offset < 0 ? 0 : pointer_->offset;
          const bool hit = recordOpen_ || wanted == chunkStored_;
          // Past the end of the node crengine refuses the pointer. The end of it is the nearest.
          const uint32_t where = recordOpen_ ? recordVisible_ : visible_;
          if (!hit) exact_ = false;
          if (empty && !frame.pre) {
            // Empty text may still go, if a block comes next: only then is this settled.
            tentative_ = true;
            tentativeOffset_ = where;
          } else {
            matchOffset_ = where;
            matchNow();
          }
        } else {
          nearOffset_ = visible_;
        }
      }
    }
    recordOpen_ = false;

    if (survives) {
      if (emitRecordOpen_) commitEmit(frame);
      frame.texts++;
      frame.kids++;
      frame.hasChild = true;
      if (elementWaiting_ && depth_ == matched_) armWhenChildrenPassed();
      if (empty && !frame.pre) {
        frame.lastEmptyText = true;
      } else {
        frame.runLeading = false;
        frame.lastEmptyText = false;
      }
    }
    emitRecordOpen_ = false;
  }

  bool chunkOwnsPointer() const {
    return pointerArmed_ && !elementArmed_ && matched_ == depth_ && depth_ == pointer_->stepCount;
  }

  void onStoredChar(const uint32_t cp, const uint32_t at, const int stored) {
    const bool excluded = excludedNesting_ > 0;
    const Frame& frame = frames_[depth_];
    const bool content = stored > 0 && !isSpaceCp(cp);

    if (mode_ == Mode::LocateText) {
      if (at >= targetOffset_ && !excluded && stored > 0 && !frame.hidden && !frame.absentFromClip && !frame.opaque) {
        if (ignoredInClip(cp)) {
          inWord_ = false;
        } else {
          if (!inWord_) wordsSeen_++;
          inWord_ = true;
          feedLocate(cp, at);
        }
      }
      return;
    }
    if (mode_ == Mode::CollectText) {
      if (at > collectLast_) {
        tracking_ = false;
        stop();
      } else if (at >= targetOffset_ && !frame.hidden && !excluded) {
        // crengine does not read ruby annotations or a <title> in a body back either.
        collectChar(cp, stored, frame);
      }
      return;
    }

    if (!excluded && content && !frame.opaque && depth_ >= minDepth_) {
      lastEligible_ = at;
      hasEligible_ = true;
      if (mode_ == Mode::EmitOffset && !found_ && !emitRecordOpen_ && at >= targetOffset_) {
        emitRecordOpen_ = true;
        emitOffset_ = chunkStored_;
      }
    }

    if (mode_ != Mode::ResolvePointer || found_) return;
    if (elementArmed_ && !excluded && content) {
      matchOffset_ = at;
      matchNow();
      return;
    }
    if (chunkOwnsPointer() && !recordOpen_ && stored > 0 && frame.texts + 1 == pointer_->textIndex) {
      const int wanted = pointer_->offset < 0 ? 0 : pointer_->offset;
      if (wanted >= chunkStored_ && wanted < chunkStored_ + stored) {
        recordOpen_ = true;
        recordVisible_ = at;
      }
    }
  }

  void commitEmit(const Frame& frame) {
    snapshot(depth_);
    snapTextIndex_ = frame.texts + 1;
    snapTextOffset_ = emitOffset_;
    snapTextTotal_ = snapTextIndex_;
    found_ = true;
    matchOffset_ = targetOffset_;
    liveDepth_ = depth_;
    armNeeds();
  }

  // ---- matching ------------------------------------------------------------------------------
  void armPointerIfComplete() {
    if (matched_ != pointer_->stepCount || pointerArmed_) return;
    pointerArmed_ = true;
    nearOffset_ = visible_;
    paragraphAtMatch_ = paragraphCount_;
    listItemAtMatch_ = listItemCount_;
    // An element xpointer lands on the first text at or after that element's start, or, with a
    // point (".4"), after that many of its children.
    if (pointer_->textIndex == 0) {
      elementWaiting_ = pointer_->offset > 0;
      elementArmed_ = !elementWaiting_;
    }
  }

  void matchNow() {
    found_ = true;
    tracking_ = false;
  }

  void armWhenChildrenPassed() {
    if (frames_[matched_].kids >= pointer_->offset) {
      elementWaiting_ = false;
      elementArmed_ = true;
    }
  }

  void dropTentative() {
    if (tentative_) exact_ = true;
    tentative_ = false;
  }

  void confirmTentative() {
    tentative_ = false;
    matchOffset_ = tentativeOffset_;
    matchNow();
  }

  void snapshot(const int depth) {
    snapCount_ = 0;
    uint16_t used = 0;
    for (int d = 1; d <= depth; d++) {
      const Frame& frame = frames_[d];
      SnapStep& step = snap_[snapCount_++];
      step.hash = hashName(names_ + frame.nameOffset, frame.nameLen);
      step.index = frame.index;
      step.nameLen = frame.nameLen;
      step.hasLater = false;
      memcpy(snapNames_ + used, names_ + frame.nameOffset, frame.nameLen);
      used = static_cast<uint16_t>(used + frame.nameLen);
    }
  }

  // What is still unknown about the found path: the "[1]" forms depend on what follows.
  void armNeeds() {
    needSteps_ = 0;
    for (int i = 0; i < snapCount_; i++) {
      if (snap_[i].index == 1) needSteps_++;
    }
    needText_ = snapTextIndex_ == 1;
    settleNeeds();
  }

  void settleNeeds() {
    if (found_ && needSteps_ == 0 && !needText_) stop();
  }

  void noteLaterSibling(const uint32_t hash) {
    if (!found_) return;
    const int level = depth_;
    if (level > snapCount_ || level - 1 > liveDepth_) return;
    SnapStep& step = snap_[level - 1];
    if (step.hash == hash && !step.hasLater) {
      step.hasLater = true;
      if (step.index == 1 && needSteps_ > 0) {
        needSteps_--;
        settleNeeds();
      }
    }
  }

  // ---- state ---------------------------------------------------------------------------------
  XML_Parser parser_ = nullptr;
  const Mode mode_;
  const int spineIndex_;
  const int spineCount_;
  CssParser* css_ = nullptr;
  const ParsedPointer* pointer_ = nullptr;
  uint32_t targetOffset_ = 0;
  uint32_t targetTagHash_ = 0;
  int targetOrdinal_ = 0;
  int elementCount_ = 0;
  uint8_t minDepth_ = 0;

  bool parseOk_ = true;
  bool stopped_ = false;
  bool tracking_ = true;
  bool inBody_ = false;
  bool bodySeen_ = false;
  bool found_ = false;
  bool pointerArmed_ = false;
  bool elementArmed_ = false;
  bool elementWaiting_ = false;
  bool hasEligible_ = false;
  bool chunkOpen_ = false;
  bool chunkHasContent_ = false;
  bool recordOpen_ = false;
  bool emitRecordOpen_ = false;
  bool needText_ = false;
  bool tentative_ = false;
  bool exact_ = true;

  int depth_ = -1;
  int matched_ = 0;
  int liveDepth_ = 0;
  int needSteps_ = 0;
  uint16_t excludedNesting_ = 0;
  uint16_t overflowNesting_ = 0;
  uint16_t usedName_ = 0;
  uint8_t usedCounters_ = 0;
  uint16_t paragraphCount_ = 0;
  uint16_t listItemCount_ = 0;
  uint16_t paragraphAtMatch_ = 0;
  uint16_t listItemAtMatch_ = 0;

  uint32_t visible_ = 0;
  uint32_t lastEligible_ = 0;
  uint32_t matchOffset_ = 0;
  uint32_t recordVisible_ = 0;
  uint32_t tentativeOffset_ = 0;
  uint32_t nearOffset_ = 0;
  int emitOffset_ = 0;

  int chunkStored_ = 0;
  int chunkRaw_ = 0;
  int chunkSpaces_ = 0;
  int chunkColumn_ = 0;
  int chunkSplitRaw_ = 0;
  int chunkSplitStored_ = 0;
  uint32_t chunkSplitVisible_ = 0;
  int chunkExtraNodes_ = 0;

  int snapCount_ = 0;
  int snapTextIndex_ = 0;
  int snapTextOffset_ = 0;
  int snapTextTotal_ = 0;

  const char* locateText_ = "";
  std::string* collectOut_ = nullptr;
  uint32_t collectLast_ = 0;
  uint32_t locateBestFirst_ = 0;
  uint32_t locateBestLast_ = 0;
  uint32_t locateBestDistance_ = 0;
  uint32_t locateMatches_ = 0;
  uint32_t wordsSeen_ = 0;
  uint16_t expectedWord_ = 0;
  uint8_t attemptCount_ = 0;
  bool inWord_ = false;
  bool locateAnchored_ = false;
  bool collectNewBlock_ = false;
  bool collectInChunk_ = false;
  Attempt attempts_[kMaxAttempts];
  CssMemo cssMemo_[kCssMemoSize];
  Frame frames_[kMaxDepth];
  Counter counters_[kMaxCounters];
  SnapStep snap_[kMaxDepth];
  char names_[kNamePool];
  char snapNames_[kNamePool];
};

// Walker state is about 2.5 KB: never on the stack of a task that is already deep.
std::unique_ptr<CreWalker> makeWalker(const CreWalker::Mode mode, const std::shared_ptr<Epub>& epub,
                                      const int spineIndex) {
  std::unique_ptr<CreWalker> walker(new (std::nothrow) CreWalker(mode, spineIndex, epub->getSpineItemsCount()));
  if (!walker || !walker->ok()) return nullptr;
  return walker;
}

// The stylesheet says what is a block and what keeps its spaces. A book opened only to sync has
// its parser empty: use the cache if there is one and the heap allows it (the parser guards that),
// and leave it as it was. A book with no cache yet goes by the element table alone.
class ScopedCss {
 public:
  explicit ScopedCss(const std::shared_ptr<Epub>& epub) : css_(epub->getCssParser()) {
    if (css_ && css_->empty() && css_->hasCache()) loaded_ = css_->loadFromCache();
  }
  ~ScopedCss() {
    if (loaded_) css_->clear();
  }
  CssParser* get() const { return css_; }

 private:
  CssParser* css_;
  bool loaded_ = false;
};

bool stream(const std::shared_ptr<Epub>& epub, const int spineIndex, CreWalker& walker) {
  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty() || !epub->readItemContentsToStream(href, walker, 1024)) return false;
  walker.finish();
  return true;
}

bool validSpine(const std::shared_ptr<Epub>& epub, const int spineIndex) {
  return epub && spineIndex >= 0 && spineIndex < epub->getSpineItemsCount();
}

std::string emitAtOffset(const std::shared_ptr<Epub>& epub, const int spineIndex, const uint32_t target,
                         const uint8_t minDepth) {
  auto walker = makeWalker(CreWalker::Mode::EmitOffset, epub, spineIndex);
  if (!walker) return "";
  walker->setTargetOffset(target, minDepth);
  ScopedCss css(epub);
  walker->setCssParser(css.get());
  if (!stream(epub, spineIndex, *walker)) return "";
  if (walker->hasMatch()) return walker->xpointer();
  if (!walker->ok()) return "";  // Could not read that far
  const uint32_t total = walker->visibleChars();
  if (target > total) return "";
  if (walker->hasEligible()) {
    // Only space, or nothing, from there to the end: the last character there is.
    return emitAtOffset(epub, spineIndex, walker->lastEligibleOffset(), minDepth);
  }
  if (minDepth > 0 || !walker->sawBody()) return "";
  // No text at all, say a chapter that is a single image.
  return bodyPointer(spineIndex, epub->getSpineItemsCount());
}

std::string emitElement(const std::shared_ptr<Epub>& epub, const int spineIndex, const uint16_t ordinal,
                        const char* tag) {
  if (!validSpine(epub, spineIndex) || ordinal == 0) return "";
  auto walker = makeWalker(CreWalker::Mode::EmitElement, epub, spineIndex);
  if (!walker) return "";
  walker->setTargetElement(tag, ordinal);
  ScopedCss css(epub);
  walker->setCssParser(css.get());
  if (!stream(epub, spineIndex, *walker)) return "";
  return walker->xpointer();
}
}  // namespace

bool ChapterXPathResolver::locateText(const std::shared_ptr<Epub>& epub, const int spineIndex, const char* text,
                                      const uint32_t anchorOffset, const uint16_t expectedWord, TextRange& out) {
  if (!validSpine(epub, spineIndex) || !text || !*text) return false;
  ScopedCss css(epub);
  uint32_t first = 0;
  uint32_t last = 0;
  {
    auto walker = makeWalker(CreWalker::Mode::LocateText, epub, spineIndex);
    if (!walker) return false;
    walker->setLocate(text, anchorOffset, expectedWord);
    walker->setCssParser(css.get());
    if (!stream(epub, spineIndex, *walker) || !walker->located()) return false;
    first = walker->locatedFirst();
    last = walker->locatedLast();
  }
  out.pos0 = emitAtOffset(epub, spineIndex, first, 0);
  out.pos1 = emitAtOffset(epub, spineIndex, last, 0);
  if (out.pos0.empty() || out.pos1.empty()) return false;
  // The character itself is what emit names; a range ends just past it. A text node's end is
  // an offset crengine takes.
  const size_t dot = out.pos1.rfind('.');
  if (dot == std::string::npos) return false;
  out.pos1 = out.pos1.substr(0, dot + 1) + std::to_string(strtoul(out.pos1.c_str() + dot + 1, nullptr, 10) + 1);

  out.text.clear();
  auto walker = makeWalker(CreWalker::Mode::CollectText, epub, spineIndex);
  if (!walker) return false;
  walker->setCollect(first, last, &out.text);
  walker->setCssParser(css.get());
  return stream(epub, spineIndex, *walker) && !out.text.empty();
}

bool ChapterXPathResolver::resolve(const std::shared_ptr<Epub>& epub, const int spineIndex, const std::string& xpointer,
                                   Resolved& out) {
  if (!validSpine(epub, spineIndex)) return false;
  std::unique_ptr<ParsedPointer> pointer(new (std::nothrow) ParsedPointer());
  if (!pointer || !pointer->parse(xpointer)) return false;
  auto walker = makeWalker(CreWalker::Mode::ResolvePointer, epub, spineIndex);
  if (!walker) return false;
  walker->setPointer(pointer.get());
  ScopedCss css(epub);
  walker->setCssParser(css.get());
  if (!stream(epub, spineIndex, *walker) || !walker->hasMatch()) return false;
  out.visibleOffset = walker->matchOffset();
  out.exact = walker->matchExact();
  out.totalVisible = walker->visibleChars();
  out.paragraphIndex = walker->paragraphs();
  out.listItemIndex = walker->listItems();
  out.targetIsListItem = pointer->targetIsListItem;
  return true;
}

std::string ChapterXPathResolver::bodyXPath(const std::shared_ptr<Epub>& epub, const int spineIndex) {
  return bodyPointer(spineIndex, epub->getSpineItemsCount());
}

std::string ChapterXPathResolver::findXPathForParagraph(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                        const uint16_t paragraphIndex) {
  return emitElement(epub, spineIndex, paragraphIndex, "p");
}

std::string ChapterXPathResolver::findXPathForListItem(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                       const uint16_t listItemIndex) {
  return emitElement(epub, spineIndex, listItemIndex, "li");
}

std::string ChapterXPathResolver::findXPathForProgress(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                       const float intraSpineProgress, const uint8_t minimumPathDepth) {
  if (!validSpine(epub, spineIndex)) return "";
  if (!(intraSpineProgress > 0.0f)) {
    if (minimumPathDepth > 0) return emitAtOffset(epub, spineIndex, 0, minimumPathDepth);
    return bodyPointer(spineIndex, epub->getSpineItemsCount());
  }
  auto counter = makeWalker(CreWalker::Mode::Count, epub, spineIndex);
  if (!counter || !stream(epub, spineIndex, *counter) || !counter->ok()) return "";
  const uint32_t total = counter->visibleChars();
  if (total == 0) return "";
  counter.reset();
  const float clamped = std::min(1.0f, intraSpineProgress);
  const uint32_t target = std::min(total - 1, static_cast<uint32_t>(clamped * static_cast<float>(total)));
  return emitAtOffset(epub, spineIndex, target, minimumPathDepth);
}

std::string ChapterXPathResolver::findXPathForVisibleTextOffset(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                                const uint32_t visibleTextOffset,
                                                                const uint8_t minimumPathDepth) {
  if (!validSpine(epub, spineIndex)) return "";
  return emitAtOffset(epub, spineIndex, visibleTextOffset, minimumPathDepth);
}
