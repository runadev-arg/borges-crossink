// Differential test: the firmware mapper against cre-oracle (KOReader's crengine), for one book.
//
//   xpdiff <bookdir> <oracle.exe> <workdir> [maxPointsPerDirection]
//
// Both sides are compared in one coordinate: the non-space characters of a spine item, in order.
// The firmware side is an independent scan of the same XHTML, not the code under test.
#include <expat.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ChapterXPathResolver.h"
#include "Epub/htmlEntities.h"
#include "Epub/parsers/VisibleOffsetRules.h"
#include "ProgressMapper.h"
#include "Utf8.h"

extern "C" {
size_t g_live = 0, g_peak = 0;
void* hmalloc(size_t n) {
  size_t* p = static_cast<size_t*>(malloc(n + 16));
  if (!p) return nullptr;
  *p = n;
  g_live += n;
  if (g_live > g_peak) g_peak = g_live;
  return reinterpret_cast<char*>(p) + 16;
}
void hfree(void* q) {
  if (!q) return;
  size_t* p = reinterpret_cast<size_t*>(static_cast<char*>(q) - 16);
  g_live -= *p;
  free(p);
}
void* hrealloc(void* q, size_t n) {
  if (!q) return hmalloc(n);
  size_t* p = reinterpret_cast<size_t*>(static_cast<char*>(q) - 16);
  void* r = hmalloc(n);
  if (r) memcpy(r, q, std::min(*p, n));
  hfree(q);
  return r;
}
}
void* operator new(size_t n) {
  void* p = hmalloc(n);
  if (!p) abort();
  return p;
}
void* operator new[](size_t n) { return operator new(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return hmalloc(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return hmalloc(n); }
void operator delete(void* p) noexcept { hfree(p); }
void operator delete[](void* p) noexcept { hfree(p); }
void operator delete(void* p, size_t) noexcept { hfree(p); }
void operator delete[](void* p, size_t) noexcept { hfree(p); }

namespace {
// Elements whose direct text crengine discards (allow_text in fb2def.h). Written out again here
// so the scan below stays apart from the code under test.
bool discardsText(const char* n) {
  static const char* const kNames[] = {"annotation", "applet",   "area",   "audio",  "author",   "col",     "colgroup",
                                       "coverpage",  "epigraph", "head",   "html",   "input",    "keygen",  "param",
                                       "part",       "poem",     "source", "stanza", "table",    "tbody",   "tfoot",
                                       "thead",      "tr",       "track",  "base",   "basefont", "bgsound", "link",
                                       "meta",       "embed",    "image",  "img",    "object",   "svg",     "g"};
  for (const char* k : kNames) {
    if (strcmp(n, k) == 0) return true;
  }
  return false;
}

struct FwChar {
  uint32_t cp;
  uint32_t visible;
};

// What the firmware's coordinate means, written out plainly.
struct FwScan {
  XML_Parser parser;
  std::vector<FwChar> content;  // Non-space characters
  uint32_t visible = 0;
  int excluded = 0;
  int bodyDepth = -1;
  int depth = 0;
  bool bodyDone = false;
  std::vector<bool> discard;

  void text(const char* s, int len) {
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    const auto* e = p + len;
    while (p < e) {
      const uint32_t cp = utf8NextCodepoint(&p);
      const uint32_t at = visible;
      if (!excluded) visible++;
      if (bodyDepth < 0 || bodyDone || excluded || (!discard.empty() && discard.back())) continue;
      if (cp != ' ' && cp != '\n' && cp != '\t' && cp != '\r') content.push_back({cp, at});
    }
  }
  static void XMLCALL start(void* d, const XML_Char* n, const XML_Char**) {
    auto* s = static_cast<FwScan*>(d);
    if (s->excluded > 0 || isVisibleOffsetExcludedTag(n)) s->excluded++;
    if (s->bodyDepth < 0 && strcmp(n, "body") == 0) s->bodyDepth = s->depth;
    s->depth++;
    s->discard.push_back(discardsText(n));
  }
  static void XMLCALL end(void* d, const XML_Char*) {
    auto* s = static_cast<FwScan*>(d);
    if (s->excluded > 0) s->excluded--;
    s->depth--;
    s->discard.pop_back();
    if (s->depth == s->bodyDepth) s->bodyDone = true;
  }
  static void XMLCALL chars(void* d, const XML_Char* c, int n) { static_cast<FwScan*>(d)->text(c, n); }
  static void XMLCALL dflt(void* d, const XML_Char* c, int n) {
    if (n < 3 || c[0] != '&' || c[n - 1] != ';') return;
    const char* v = lookupHtmlEntity(c, n);
    static_cast<FwScan*>(d)->text(v ? v : c, v ? static_cast<int>(strlen(v)) : n);
  }
  bool ok = true;
  void run(const std::string& xml) {
    parser = XML_ParserCreate(nullptr);
    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, start, end);
    XML_SetCharacterDataHandler(parser, chars);
    XML_SetDefaultHandlerExpand(parser, dflt);
    ok = XML_Parse(parser, xml.data(), static_cast<int>(xml.size()), XML_TRUE) != XML_STATUS_ERROR;
    XML_ParserFree(parser);
  }
};

std::vector<uint32_t> decode(const std::string& s) {
  std::vector<uint32_t> out;
  const auto* p = reinterpret_cast<const unsigned char*>(s.c_str());
  while (*p) out.push_back(utf8NextCodepoint(&p));
  return out;
}

std::string unescape(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i] == '\\' && i + 1 < in.size()) {
      i++;
      out += in[i] == 't' ? '\t' : in[i] == 'n' ? '\n' : in[i] == 'r' ? '\r' : in[i];
    } else {
      out += in[i];
    }
  }
  return out;
}

std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> f;
  size_t a = 0;
  while (true) {
    const size_t b = line.find('\t', a);
    f.push_back(line.substr(a, b == std::string::npos ? b : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return f;
}

bool isSpace(const uint32_t c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }

struct Node {
  int frag;
  std::string xp;  // Ends in ".0"
  std::vector<uint32_t> text;
  int ordinalBefore;
  bool internalText;  // Inside something the firmware's coordinate excludes
};

struct Stats {
  int total = 0, exact = 0, notFound = 0, wrong = 0, canonical = 0, nearest = 0;
  int maxDelta = 0;
};

// An oracle ordinal to a firmware content index, walking the two sequences together. They differ
// only where one side has text the other does not.
struct Alignment {
  std::vector<int> oracleToFw;
  std::vector<int> fwToOracle;
  int unmatchedOracle = 0;
  int unmatchedFw = 0;
};

// crengine maps the C1 controls of a numeric reference (&#150;) to what windows-1252 has there.
bool sameChar(const uint32_t oracle, const uint32_t fw) { return oracle == fw || (fw >= 0x80 && fw < 0xA0); }

Alignment align(const std::vector<uint32_t>& o, const std::vector<bool>& skip, const std::vector<FwChar>& f) {
  Alignment a;
  a.oracleToFw.assign(o.size() + 1, -1);
  a.fwToOracle.assign(f.size() + 1, -1);
  size_t i = 0, j = 0;
  const auto same = [&](size_t x, size_t y) {
    for (int k = 0; k < 6; k++) {
      if (x + k >= o.size() || y + k >= f.size()) return x + k >= o.size() && y + k >= f.size();
      if (!sameChar(o[x + k], f[y + k].cp)) return false;
    }
    return true;
  };
  while (i < o.size() && j < f.size()) {
    // What the firmware's coordinate does not count: it lands on what follows.
    if (skip[i]) {
      i++;
      continue;
    }
    if (sameChar(o[i], f[j].cp)) {
      a.oracleToFw[i] = static_cast<int>(j);
      a.fwToOracle[j] = static_cast<int>(i);
      i++;
      j++;
      continue;
    }
    bool moved = false;
    for (size_t k = 1; k < 8192 && !moved; k++) {
      if (i + k <= o.size() && same(i + k, j)) {
        a.unmatchedOracle += static_cast<int>(k);
        i += k;
        moved = true;
      } else if (j + k <= f.size() && same(i, j + k)) {
        a.unmatchedFw += static_cast<int>(k);
        j += k;
        moved = true;
      }
    }
    if (!moved) {
      a.unmatchedOracle++;
      a.unmatchedFw++;
      i++;
      j++;
    }
  }
  for (; i < o.size(); i++) {
    if (!skip[i]) a.unmatchedOracle++;
  }
  a.unmatchedFw += static_cast<int>(f.size() - j);
  a.oracleToFw[o.size()] = static_cast<int>(f.size());
  a.fwToOracle[f.size()] = static_cast<int>(o.size());
  // A position in text only one side has lands on what comes next.
  for (int x = static_cast<int>(o.size()) - 1; x >= 0; x--) {
    if (a.oracleToFw[x] < 0)
      a.oracleToFw[x] = a.oracleToFw[x + 1] < 0 ? static_cast<int>(f.size()) : a.oracleToFw[x + 1];
  }
  for (int x = static_cast<int>(f.size()) - 1; x >= 0; x--) {
    if (a.fwToOracle[x] < 0)
      a.fwToOracle[x] = a.fwToOracle[x + 1] < 0 ? static_cast<int>(o.size()) : a.fwToOracle[x + 1];
  }
  return a;
}

// First content index at or after a visible offset.
int fwIndexAt(const std::vector<FwChar>& f, const uint32_t visible) {
  const auto it =
      std::lower_bound(f.begin(), f.end(), visible, [](const FwChar& c, const uint32_t v) { return c.visible < v; });
  return static_cast<int>(it - f.begin());
}

std::string readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// What the plugin compares: comparableText() in borges.koplugin/annotationadapter.lua.
std::string comparable(const std::string& in) {
  std::string out;
  bool space = false;
  for (size_t i = 0; i < in.size(); i++) {
    const auto c = static_cast<unsigned char>(in[i]);
    if (c == 0xC2 && i + 1 < in.size() && static_cast<unsigned char>(in[i + 1]) == 0xA0) {
      space = true;
      i++;
      continue;
    }
    if (c == 0xC2 && i + 1 < in.size() && static_cast<unsigned char>(in[i + 1]) == 0xAD) {
      i++;
      continue;
    }
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
      space = true;
      continue;
    }
    if (space && !out.empty()) out += ' ';
    space = false;
    out += static_cast<char>(c);
  }
  return out;
}

std::string noSpaces(const std::string& in) {
  std::string out;
  const auto* p = reinterpret_cast<const unsigned char*>(in.c_str());
  while (*p) {
    const auto* start = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    const bool ignored = isSpace(cp) || cp == 0xA0 || cp == 0xAD || cp == 0xFEFF || (cp >= 0x2000 && cp <= 0x200B) ||
                         cp == 0x202F || cp == 0x205F || cp == 0x3000;
    if (!ignored) out.append(reinterpret_cast<const char*>(start), p - start);
  }
  return out;
}

// The reader adds a bullet or "12." to a list item, and these are in the clipping alone.
bool sameButForListMarkers(const std::string& read, const std::string& clip) {
  size_t r = 0, c = 0;
  while (c < clip.size()) {
    if (r < read.size() && read[r] == clip[c]) {
      r++;
      c++;
      continue;
    }
    if (clip.compare(c, 3, "\xE2\x80\xA2") == 0) {
      c += 3;
      continue;
    }
    size_t d = c;
    while (d < clip.size() && clip[d] >= '0' && clip[d] <= '9') d++;
    if (d > c && d < clip.size() && clip[d] == '.') {
      c = d + 1;
      continue;
    }
    // Backing up over digits both sides began with: "1.2" against a marker "12."
    return false;
  }
  return r == read.size();
}

// Never the text: its length, and what kind of character the first difference is.
std::string kindOf(const std::string& s, size_t i) {
  if (i >= s.size()) return "end";
  const auto c = static_cast<unsigned char>(s[i]);
  if (c == ' ') return "space";
  if (c >= 0x80) return "nonascii";
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return "letter";
  if (c >= '0' && c <= '9') return "digit";
  return std::string("punct") + static_cast<char>(c);
}

std::string firstDifference(const std::string& a, const std::string& b) {
  size_t i = 0;
  while (i < a.size() && i < b.size() && a[i] == b[i]) i++;
  return std::to_string(i) + "/" + std::to_string(a.size()) + "," + std::to_string(b.size()) + " " + kindOf(a, i) +
         "|" + kindOf(b, i);
}

std::string clipShape(const std::string& text) {
  std::string shape;
  if (text.find("[Image") != std::string::npos) shape += "image-standin ";
  if (text.find("\xC2\xAD") != std::string::npos) shape += "shy ";
  if (text.find("\xC2\xA0") != std::string::npos) shape += "nbsp ";
  if (text.find('\n') != std::string::npos) shape += "paragraphs ";
  return shape.empty() ? "plain" : shape;
}

// Why a clipping was not found, in structure only: how far it matches the chapter from anywhere,
// the kind of character on each side where it stops, and where that match is from the anchor.
std::string whereItStops(const std::string& clip, const std::vector<FwChar>& content, const uint32_t anchor,
                         const uint16_t word) {
  const std::vector<uint32_t> wanted = decode(noSpaces(clip));
  size_t best = 0, bestAt = 0;
  for (size_t i = 0; i < content.size(); i++) {
    size_t k = 0;
    while (k < wanted.size() && i + k < content.size() && content[i + k].cp == wanted[k]) k++;
    if (k > best) {
      best = k;
      bestAt = i;
    }
  }
  const auto kind = [](const uint32_t cp) -> std::string {
    if (cp == 0) return "end";
    if (cp < 0x80) {
      if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) return "letter";
      if (cp >= '0' && cp <= '9') return "digit";
      return std::string("'") + static_cast<char>(cp) + "'";
    }
    char hex[16];
    snprintf(hex, sizeof(hex), "U+%04X", cp);
    return hex;
  };
  const uint32_t clipCp = best < wanted.size() ? wanted[best] : 0;
  const uint32_t srcCp = bestAt + best < content.size() ? content[bestAt + best].cp : 0;
  const long fromAnchor = best > 0 ? static_cast<long>(content[bestAt].visible) - static_cast<long>(anchor) : 0;
  std::string shape;
  if (getenv("XPDIFF_SHOW_SHAPE")) {
    // Letters and digits masked: what is left is structure, safe for a book under copyright.
    const auto mask = [](const uint32_t cp) -> char {
      if (cp >= 'a' && cp <= 'z') return 'x';
      if (cp >= 'A' && cp <= 'Z') return 'X';
      if (cp >= '0' && cp <= '9') return '9';
      if (cp < 0x80) return static_cast<char>(cp);
      if (cp == 0xA0) return '_';
      return cp >= 0x2000 && cp < 0x2070 ? '~' : 'u';
    };
    shape = " clipShape=";
    for (size_t i = best > 12 ? best - 12 : 0; i < wanted.size() && i < best + 24; i++) shape += mask(wanted[i]);
    shape += " srcShape=";
    for (size_t i = bestAt + (best > 12 ? best - 12 : 0); i < content.size() && i < bestAt + best + 24; i++)
      shape += mask(content[i].cp);
  }
  return "matched=" + std::to_string(best) + "/" + std::to_string(wanted.size()) + " clip=" + kind(clipCp) +
         " src=" + kind(srcCp) + " fromAnchor=" + std::to_string(fromAnchor) + " word=" + std::to_string(word) + shape;
}

__attribute__((noinline)) size_t paintStack() {
  constexpr size_t kSize = 96 * 1024;
  volatile unsigned char* p = static_cast<volatile unsigned char*>(__builtin_alloca(kSize));
  for (size_t i = 0; i < kSize; i++) p[i] = 0xA5;
  return reinterpret_cast<size_t>(p);
}

__attribute__((noinline)) size_t measureStack(const size_t low) {
  constexpr size_t kSize = 96 * 1024;
  const auto* p = reinterpret_cast<const unsigned char*>(low);
  size_t i = 0;
  while (i < kSize && p[i] == 0xA5) i++;
  return kSize - i;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: xpdiff bookdir oracle workdir [max]\n");
    return 2;
  }
  const std::string dir = argv[1];
  const std::string oracle = argv[2];
  const std::string work = argv[3];
  const int maxPoints = argc > 4 ? atoi(argv[4]) : 2000;
  const bool verbose = getenv("XPDIFF_VERBOSE") != nullptr;
  const std::string name = dir.substr(dir.find_last_of("/\\") + 1);

  std::vector<std::string> spine;
  for (int i = 0;; i++) {
    char path[32];
    snprintf(path, sizeof(path), "/spine/%04d.xhtml", i);
    std::ifstream in(dir + path, std::ios::binary);
    if (!in) break;
    std::stringstream ss;
    ss << in.rdbuf();
    spine.push_back(ss.str());
  }

  const std::string enumPath = work + "/" + name + ".enum";
  const char* oracleExtra = getenv("XPDIFF_ORACLE_ARGS");
  const std::string extra = oracleExtra ? std::string(" ") + oracleExtra : "";
  std::string cmd = "\"\"" + oracle + "\" \"" + dir + "/book.epub\"" + extra + " --enum \"" + enumPath + "\"\"";
  if (system(cmd.c_str()) != 0) {
    printf("%s\tORACLE-FAILED\n", name.c_str());
    return 1;
  }

  auto epub = std::make_shared<Epub>(spine);
  {
    std::vector<std::string> sheets;
    for (int i = 0;; i++) {
      char path[32];
      snprintf(path, sizeof(path), "/css/%04d.css", i);
      std::ifstream in(dir + path, std::ios::binary);
      if (!in) break;
      std::stringstream ss;
      ss << in.rdbuf();
      sheets.push_back(ss.str());
    }
    if (!getenv("XPDIFF_NO_CSS")) epub->loadCss(sheets);
  }

  // A walker with its answer stops reading: the exit of a reader cannot afford the whole chapter.
  // Not always possible, since the form of an xpointer depends on whether a later sibling exists: a
  // chapter that is one <div> is read to its end to say so. So what counts is that some chapters do
  // stop early, which they never did while the walker told the inflater it took everything.
  {
    size_t checked = 0;
    size_t stopped = 0;
    for (int i = 0; i < static_cast<int>(spine.size()); ++i) {
      if (spine[i].size() < 16 * 1024) continue;
      Epub::bytesFed = 0;
      if (ChapterXPathResolver::findXPathForVisibleTextOffset(epub, i, 0).empty()) continue;
      ++checked;
      if (Epub::bytesFed <= spine[i].size() / 2) ++stopped;
    }
    printf("  earlystop checked=%zu stopped=%zu\n", checked, stopped);
  }

  std::vector<std::unique_ptr<FwScan>> scans;
  for (const auto& s : spine) {
    scans.push_back(std::make_unique<FwScan>());
    scans.back()->run(s);
  }

  std::vector<Node> nodes;
  struct PagePoint {
    std::string xp;
  };
  std::vector<PagePoint> pages;
  std::map<int, std::vector<uint32_t>> oracleContent;
  std::map<int, std::vector<bool>> oracleSkip;
  {
    std::ifstream in(enumPath, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      const auto f = split(line);
      if (f[0] == "N") {
        Node n;
        n.frag = atoi(f[1].c_str());
        n.xp = f[2];
        n.text = decode(unescape(f[4]));
        auto& c = oracleContent[n.frag];
        n.ordinalBefore = static_cast<int>(c.size());
        n.internalText = false;
        for (const char* excluded : {"/style", "/script", "/title", "/rp", "/rt"}) {
          if (n.xp.find(excluded) != std::string::npos) n.internalText = true;
        }
        for (const uint32_t cp : n.text) {
          if (!isSpace(cp)) {
            c.push_back(cp);
            oracleSkip[n.frag].push_back(n.internalText);
          }
        }
        nodes.push_back(std::move(n));
      } else if (f[0] == "P" && f.size() > 2 && !f[2].empty()) {
        pages.push_back({f[2]});
      }
    }
  }

  std::map<int, Alignment> alignments;
  int unmatchedOracle = 0, unmatchedFw = 0;
  for (size_t s = 0; s < spine.size(); s++) {
    alignments[static_cast<int>(s + 1)] =
        align(oracleContent[static_cast<int>(s + 1)], oracleSkip[static_cast<int>(s + 1)], scans[s]->content);
    unmatchedOracle += alignments[static_cast<int>(s + 1)].unmatchedOracle;
    unmatchedFw += alignments[static_cast<int>(s + 1)].unmatchedFw;
  }

  FILE* dumpPages = nullptr;
  if (getenv("XPDIFF_DUMP_PAGES")) dumpPages = fopen((work + "/" + name + ".dump").c_str(), "wb");

  // ---- KOReader to device ----------------------------------------------------------------------
  Stats k2d;
  std::string examples;
  int shown = 0;
  size_t peakHeap = 0, peakStack = 0;
  const auto check = [&](const std::string& xp, const int frag, const int oracleOrdinal, const std::string& kind) {
    if (frag < 1 || frag > static_cast<int>(spine.size())) return;
    // A spine item with no XHTML body: crengine's placeholder for it is not in the book.
    if (scans[frag - 1]->bodyDepth < 0) return;
    k2d.total++;
    KOReaderPosition pos;
    pos.xpath = xp;
    pos.percentage = 0.5f;
    const size_t liveBefore = g_live;
    g_peak = g_live;
    const size_t low = paintStack();
    const CrossPointPosition got = ProgressMapper::toCrossPoint(epub, pos, -1, 1);
    peakStack = std::max(peakStack, measureStack(low));
    peakHeap = std::max(peakHeap, g_peak - liveBefore);
    const auto& f = scans[frag - 1]->content;
    const int want = alignments[frag].oracleToFw[std::min<int>(oracleOrdinal, alignments[frag].oracleToFw.size() - 1)];
    if (!got.hasVisibleTextOffset || got.spineIndex != frag - 1) {
      k2d.notFound++;
      if (shown++ < 400) examples += "  k2d not-found " + kind + " " + xp + "\n";
      return;
    }
    const int have = fwIndexAt(f, got.visibleTextOffset);
    if (have == want) {
      k2d.exact++;
      // For checking a real layout in the simulator: where a KOReader page start lands.
      if (kind == "page" && dumpPages) fprintf(dumpPages, "%d\t%u\n", frag, got.visibleTextOffset);
    } else {
      k2d.wrong++;
      k2d.maxDelta = std::max(k2d.maxDelta, std::abs(have - want));
      if (shown++ < 400) {
        examples +=
            "  k2d wrong " + kind + " " + xp + " fw=" + std::to_string(have) + " want=" + std::to_string(want) + "\n";
      }
    }
  };

  // Sampling keeps every book's nodes represented: each node's start, and offsets through it.
  int budget = maxPoints;
  const int perNode = std::max(3, nodes.empty() ? 3 : maxPoints / static_cast<int>(nodes.size()));
  for (const Node& n : nodes) {
    if (n.internalText) continue;
    const std::string base = n.xp.substr(0, n.xp.rfind('.') + 1);
    const int len = static_cast<int>(n.text.size());
    const int step = std::max(1, len / perNode);
    int ordinal = n.ordinalBefore;
    int nextSample = 0;
    for (int off = 0; off < len && budget > 0; off++) {
      if (off == nextSample) {
        // A space lands on what follows it, on both sides.
        check(base + std::to_string(off), n.frag, ordinal, "node");
        budget--;
        nextSample += step;
      }
      if (!isSpace(n.text[off])) ordinal++;
    }
    if (budget-- > 0) check(base + std::to_string(len), n.frag, ordinal, "node-end");
  }
  // Every page start, the positions KOReader really syncs. Their ordinals come from the oracle.
  const std::string pageIn = work + "/" + name + ".pages";
  const std::string pageOut = work + "/" + name + ".pages.out";
  {
    std::ofstream out(pageIn, std::ios::binary);
    for (const auto& p : pages) out << p.xp << "\n";
  }
  cmd = "\"\"" + oracle + "\" \"" + dir + "/book.epub\"" + extra + " --resolve \"" + pageIn + "\" --out \"" + pageOut +
        "\"\"";
  if (!pages.empty() && system(cmd.c_str()) == 0) {
    std::ifstream in(pageOut, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      const auto f = split(line);
      if (f.size() >= 10 && f[2] == "1" && atoi(f[8].c_str()) >= 0)
        check(f[1], atoi(f[7].c_str()), atoi(f[8].c_str()), "page");
    }
  }

  if (dumpPages) fclose(dumpPages);

  // ---- Device to KOReader ----------------------------------------------------------------------
  struct Emitted {
    int frag;
    int fwIndex;
    uint32_t visible;
    std::string xp;
  };
  std::vector<Emitted> emitted;
  Stats d2k;
  int roundTripBad = 0, roundTripTotal = 0;
  int parserSpines = 0, parserFailed = 0, parserDelta = 0, pagesTotal = 0, pagesBad = 0;
  struct ClipSample {
    int spine;
    uint32_t anchor;
    uint16_t word;
    std::string text;
  };
  std::vector<ClipSample> clips;
  std::string d2kIn = work + "/" + name + ".d2k";
  std::string d2kOut = work + "/" + name + ".d2k.out";
  {
    std::ofstream out(d2kIn, std::ios::binary);
    size_t totalContent = 0;
    for (const auto& s : scans) totalContent += s->content.size();
    const size_t stride = std::max<size_t>(1, totalContent / std::max(1, maxPoints));
    for (size_t s = 0; s < scans.size(); s++) {
      const auto& f = scans[s]->content;
      for (size_t i = 0; i < f.size(); i += stride) {
        const size_t liveBefore = g_live;
        g_peak = g_live;
        const size_t low = paintStack();
        const std::string xp =
            ChapterXPathResolver::findXPathForVisibleTextOffset(epub, static_cast<int>(s), f[i].visible);
        peakStack = std::max(peakStack, measureStack(low));
        peakHeap = std::max(peakHeap, g_peak - liveBefore);
        d2k.total++;
        if (xp.empty()) {
          d2k.notFound++;
          if (shown++ < 400)
            examples += "  d2k empty spine=" + std::to_string(s) + " visible=" + std::to_string(f[i].visible) + "\n";
          continue;
        }
        emitted.push_back({static_cast<int>(s + 1), static_cast<int>(i), f[i].visible, xp});
        out << xp << "\n";
        // Round trip inside the firmware: the same character comes back.
        ChapterXPathResolver::Resolved r;
        roundTripTotal++;
        if (!ChapterXPathResolver::resolve(epub, static_cast<int>(s), xp, r) ||
            fwIndexAt(f, r.visibleOffset) != static_cast<int>(i)) {
          roundTripBad++;
          if (shown++ < 400) examples += "  roundtrip " + xp + "\n";
        }
      }
      // Where the real parser starts each page, the path a saved position takes: the offset the
      // reader keeps, the xpointer KOReader gets, and the page the reader finds it on again.
      const char* pagesTool = getenv("XPDIFF_PAGES_TOOL");
      if (pagesTool && scans[s]->bodyDepth >= 0) {
        char spinePath[32];
        snprintf(spinePath, sizeof(spinePath), "/spine/%04d.xhtml", static_cast<int>(s));
        const std::string pgPath = work + "/" + name + ".pg";
        std::string pagesCmd = "\"\"" + std::string(pagesTool) + "\" \"" + dir + spinePath + "\" \"" + pgPath + "\"";
        for (int i = 0;; i++) {
          char cssPath[32];
          snprintf(cssPath, sizeof(cssPath), "/css/%04d.css", i);
          if (!std::ifstream(dir + cssPath)) break;
          pagesCmd += " \"" + dir + cssPath + "\"";
        }
        pagesCmd += "\"";
        if (system(pagesCmd.c_str()) == 0) {
          std::vector<uint32_t> lut;
          std::ifstream pg(pgPath, std::ios::binary);
          std::string line;
          while (std::getline(pg, line)) {
            const auto fields = split(line);
            if (fields[0] == "T") {
              parserSpines++;
              if (fields[2] != "1") parserFailed++;
              if (strtoul(fields[1].c_str(), nullptr, 10) != scans[s]->visible) {
                parserDelta++;
                if (shown++ < 400) {
                  examples += "  count spine=" + std::to_string(s) + " parser=" + fields[1] +
                              " scan=" + std::to_string(scans[s]->visible) + "\n";
                }
              }
            } else if (fields[0] == "P") {
              lut.push_back(strtoul(fields[2].c_str(), nullptr, 10));
            } else if (fields[0] == "C" && fields.size() > 3) {
              clips.push_back({static_cast<int>(s), static_cast<uint32_t>(strtoul(fields[1].c_str(), nullptr, 10)),
                               static_cast<uint16_t>(atoi(fields[2].c_str())), unescape(fields[3])});
            }
          }
          const auto pageOf = [&](const uint32_t v) {
            size_t page = 0;
            for (size_t i = 0; i < lut.size() && lut[i] <= v; i++) {
              page = i;
              if (lut[i] == v) break;
            }
            return page;
          };
          for (size_t page = 0; page < lut.size(); page++) {
            if (page > 0 && lut[page] == lut[page - 1]) continue;  // Pages with no text of their own
            // The first character after a page start that holds one: past it, the next page.
            const int index = fwIndexAt(f, lut[page]);
            if (index >= static_cast<int>(f.size())) continue;
            if (page + 1 < lut.size() && f[index].visible >= lut[page + 1]) continue;
            pagesTotal++;
            const std::string xp =
                ChapterXPathResolver::findXPathForVisibleTextOffset(epub, static_cast<int>(s), lut[page]);
            ChapterXPathResolver::Resolved r;
            if (xp.empty() || !ChapterXPathResolver::resolve(epub, static_cast<int>(s), xp, r) ||
                pageOf(r.visibleOffset) != page) {
              pagesBad++;
              if (shown++ < 400) examples += "  page round trip spine=" + std::to_string(s) + " " + xp + "\n";
              continue;
            }
            emitted.push_back({static_cast<int>(s + 1), index, lut[page], xp});
            out << xp << "\n";
            d2k.total++;
          }
        }
      }
      // Chapter ends, and offsets on a space.
      if (!f.empty()) {
        const std::string xp =
            ChapterXPathResolver::findXPathForVisibleTextOffset(epub, static_cast<int>(s), scans[s]->visible);
        if (!xp.empty()) {
          emitted.push_back({static_cast<int>(s + 1), static_cast<int>(f.size() - 1), scans[s]->visible, xp});
          out << xp << "\n";
          d2k.total++;
        }
      } else {
        const std::string xp = ChapterXPathResolver::findXPathForVisibleTextOffset(epub, static_cast<int>(s), 0);
        if (!xp.empty()) {
          emitted.push_back({static_cast<int>(s + 1), -1, 0, xp});
          out << xp << "\n";
          d2k.total++;
        }
      }
    }
  }
  cmd = "\"\"" + oracle + "\" \"" + dir + "/book.epub\"" + extra + " --resolve \"" + d2kIn + "\" --out \"" + d2kOut +
        "\"\"";
  if (!emitted.empty() && system(cmd.c_str()) == 0) {
    std::ifstream in(d2kOut, std::ios::binary);
    std::string line;
    size_t k = 0;
    while (std::getline(in, line) && k < emitted.size()) {
      const auto f = split(line);
      const Emitted& e = emitted[k++];
      if (f.size() < 3 || f[2] != "1") {
        d2k.wrong++;
        if (shown++ < 400) examples += "  d2k rejected " + e.xp + "\n";
        continue;
      }
      if (f[3] == e.xp)
        d2k.canonical++;
      else if (shown++ < 400)
        examples += "  d2k non-canonical " + e.xp + " vs " + f[3] + "\n";
      const int frag = atoi(f[7].c_str());
      const int ordinal = atoi(f[8].c_str());
      if (e.fwIndex < 0) {
        if (frag == e.frag)
          d2k.exact++;
        else
          d2k.wrong++;
        continue;
      }
      const int want = alignments[e.frag].fwToOracle[e.fwIndex];
      if (frag == e.frag && ordinal == want) {
        d2k.exact++;
      } else {
        d2k.wrong++;
        d2k.maxDelta = std::max(d2k.maxDelta, std::abs(ordinal - want));
        if (shown++ < 400) {
          examples += "  d2k wrong " + e.xp + " frag=" + std::to_string(frag) + " ordinal=" + std::to_string(ordinal) +
                      " want=" + std::to_string(want) + "\n";
        }
      }
    }
  }

  // ---- Clippings ---------------------------------------------------------------------------------
  // A selection made by the real parser and builder, located, and read back the way KOReader
  // reads the range. Only shapes and counts are ever printed: this text is the book's.
  int clipTotal = 0, clipRefused = 0, clipRejected = 0, clipExact = 0, clipOtherText = 0, clipSentAsIs = 0;
  size_t clipPeakHeap = 0;
  if (!clips.empty()) {
    struct Located {
      size_t clip;
      std::string text;
    };
    std::vector<Located> located;
    const std::string rangesIn = work + "/" + name + ".ranges";
    const std::string rangesOut = rangesIn + ".out";
    {
      std::ofstream out(rangesIn, std::ios::binary);
      for (size_t i = 0; i < clips.size(); i++) {
        clipTotal++;
        ChapterXPathResolver::TextRange range;
        const size_t liveBefore = g_live;
        g_peak = g_live;
        const bool ok = ChapterXPathResolver::locateText(epub, clips[i].spine, clips[i].text.c_str(), clips[i].anchor,
                                                         clips[i].word, range);
        clipPeakHeap = std::max(clipPeakHeap, g_peak - liveBefore);
        if (!ok) {
          clipRefused++;
          if (shown++ < 400) {
            examples += "  clip refused spine=" + std::to_string(clips[i].spine) +
                        " len=" + std::to_string(clips[i].text.size()) + " class=" + clipShape(clips[i].text) + " " +
                        whereItStops(clips[i].text, scans[clips[i].spine]->content, clips[i].anchor, clips[i].word) +
                        (getenv("XPDIFF_SHOW_TEXT") ? " [" + clips[i].text + "]" : std::string()) + "\n";
          }
          continue;
        }
        out << range.pos0 << "\t" << range.pos1 << "\n";
        located.push_back({i, range.text});
      }
    }
    cmd = "\"\"" + oracle + "\" \"" + dir + "/book.epub\"" + extra + " --ranges \"" + rangesIn + "\" --out \"" +
          rangesOut + "\"\"";
    if (!located.empty() && system(cmd.c_str()) == 0) {
      std::ifstream in(rangesOut, std::ios::binary);
      std::string line;
      size_t k = 0;
      while (std::getline(in, line) && k < located.size()) {
        const auto f = split(line);
        const Located& l = located[k++];
        const std::string& clip = clips[l.clip].text;
        if (f.size() < 3 || f[1] != "1") {
          clipRejected++;
          continue;
        }
        const std::string crengine = unescape(f[2]);
        // comparableText() in borges.koplugin/annotationadapter.lua decides this on the device.
        const std::string read = comparable(crengine);
        if (read == comparable(l.text))
          clipExact++;
        else if (shown++ < 400)
          examples += "  clip spacing differs spine=" + std::to_string(clips[l.clip].spine) +
                      " at=" + firstDifference(read, comparable(l.text)) +
                      (getenv("XPDIFF_SHOW_TEXT") ? " [" + read + "] vs [" + comparable(l.text) + "]" : std::string()) +
                      "\n";
        if (read == comparable(clip)) clipSentAsIs++;
        // And what is sent is the clipping's text, not something else nearby.
        if (!sameButForListMarkers(noSpaces(utf8ComposeNfc(read)), noSpaces(clip))) {
          clipOtherText++;
          if (shown++ < 400)
            examples += "  clip other text spine=" + std::to_string(clips[l.clip].spine) +
                        " len=" + std::to_string(clip.size()) + "/" + std::to_string(crengine.size()) +
                        " at=" + firstDifference(noSpaces(crengine), noSpaces(clip)) +
                        (getenv("XPDIFF_SHOW_TEXT") ? " [" + crengine + "] vs [" + clip + "]" : std::string()) + "\n";
        }
      }
    }
    printf("  clips total=%d exact=%d refused=%d rejected=%d otherText=%d matchedWithoutRewriting=%d peakHeap=%zu\n",
           clipTotal, clipExact, clipRefused, clipRejected, clipOtherText, clipSentAsIs, clipPeakHeap);
  }

  // book  k2d: total exact notfound wrong maxdelta  d2k: total exact empty wrong canonical maxdelta  rt: total bad
  // only-oracle only-fw  peakHeap peakStack
  printf("%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%zu\t%zu\n", name.c_str(), k2d.total,
         k2d.exact, k2d.notFound, k2d.wrong, k2d.maxDelta, d2k.total, d2k.exact, d2k.notFound, d2k.wrong, d2k.canonical,
         d2k.maxDelta, roundTripTotal, roundTripBad, unmatchedOracle, unmatchedFw, peakHeap, peakStack);
  if (getenv("XPDIFF_PAGES_TOOL")) {
    printf("  pages spines=%d parseFailed=%d countDiffers=%d pageStarts=%d roundTripBad=%d\n", parserSpines,
           parserFailed, parserDelta, pagesTotal, pagesBad);
  }
  if (verbose) fputs(examples.c_str(), stdout);
  return 0;
}
