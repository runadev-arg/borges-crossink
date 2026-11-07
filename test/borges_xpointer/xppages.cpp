// Runs the firmware's real ChapterHtmlSlimParser and ParsedText over one spine item, with a
// fixed-width stand-in for the font, and writes what the section cache would keep:
//
//   xppages <spine.xhtml> <out> [css ...]
//   T <visible text length> <parse ok>
//   P <page> <visible offset of its first word>
//   C <anchor offset> <first word on its page> <text>
//   W <page> <inserted hyphen> <first of its line> <word>     (XPPAGES_WORDS only)
//
// XPPAGES_LAYOUT=<width>x<height>x<advance per char> picks another layout than portrait at 9 px.
//
// C lines are selections over the page's words, made by the real ClipTextBuilder, the same way
// the reader builds its word store. Their text is the book's: this output stays in scratch.
//
// xpdiff reads this to hold the walker's count and each page start to real crengine.
#include <Epub.h>
#include <Epub/Page.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "Epub/css/CssParser.h"
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#include "clippings/ClipTextBuilder.h"

extern void (*parserTextBlockHook)(const TextBlock*, const std::vector<std::string>&, const std::vector<int16_t>&,
                                   const std::vector<uint8_t>&);

namespace {
struct LineWords {
  std::vector<std::string> words;
  std::vector<int16_t> x;
  std::vector<uint8_t> flags;
};
std::map<const TextBlock*, LineWords> g_lines;

struct PageWords {
  uint32_t anchor = 0;
  struct Word {
    std::string text;
    int x, y;
    bool hyphen;
    uint16_t index;
  };
  std::vector<Word> words;
};

std::string escape(const std::string& in) {
  std::string out;
  for (const char c : in) {
    if (c == '\\')
      out += "\\\\";
    else if (c == '\t')
      out += "\\t";
    else if (c == '\n')
      out += "\\n";
    else if (c == '\r')
      out += "\\r";
    else
      out += c;
  }
  return out;
}

// Deterministic, so a mismatch can be run again.
uint32_t g_seed = 12345;
uint32_t nextRandom() {
  g_seed = g_seed * 1664525u + 1013904223u;
  return g_seed >> 8;
}

void writeSelections(FILE* out, const std::vector<PageWords>& pages, const int perChapter) {
  if (pages.empty()) return;
  for (int n = 0; n < perChapter; n++) {
    const size_t pageIndex = nextRandom() % pages.size();
    const PageWords& page = pages[pageIndex];
    if (page.words.empty()) continue;
    // The word store spans this page and the next, as the reader's does.
    ClipWordStore store;
    std::vector<uint16_t> order;
    for (size_t p = pageIndex; p < pages.size() && p <= pageIndex + 1; p++) {
      for (const auto& w : pages[p].words) {
        WordRef ref;
        ref.x = w.x;
        ref.y = w.y;
        ref.w = static_cast<int>(w.text.size()) * 9;
        ref.h = 16;
        ref.pageIdx = static_cast<int>(p - pageIndex);
        ref.pageWordIndex = w.index;
        ref.endsWithInsertedHyphen = w.hyphen;
        if (!store.appendText(ref, w.text.c_str())) break;
        order.push_back(static_cast<uint16_t>(store.words.size()));
        store.words.push_back(ref);
      }
    }
    if (order.empty()) continue;
    static const int kLengths[] = {1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377};
    const int length = kLengths[nextRandom() % (sizeof(kLengths) / sizeof(kLengths[0]))];
    const int from = static_cast<int>(nextRandom() % page.words.size());
    const int to = std::min<int>(from + length - 1, static_cast<int>(order.size()) - 1);
    const ClippingResult result = ClipTextBuilder::build(store, order.data(), from, to, 0, 1);
    if (result.text.empty() || result.text.size() > 4096) continue;
    fprintf(out, "C\t%u\t%u\t%s\n", page.anchor, result.startPageWordIndex, escape(result.text).c_str());
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const std::string path = argv[1];
  std::vector<uint32_t> pages;
  Epub epub;
  GfxRenderer renderer;
  renderer.textAdvancePerChar = 9;
  CssParser css{""};
  for (int i = 3; i < argc; i++) {
    std::ifstream in(argv[i], std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    HalFile file(ss.str());
    css.loadFromStream(file);
  }
  int viewportWidth = 480;
  int viewportHeight = 800;
  if (const char* layout = getenv("XPPAGES_LAYOUT")) {
    int advance = 9;
    if (sscanf(layout, "%dx%dx%d", &viewportWidth, &viewportHeight, &advance) == 3)
      renderer.textAdvancePerChar = advance;
  }
  const bool wantWords = getenv("XPPAGES_WORDS") != nullptr;
  struct ListedWord {
    size_t page;
    bool hyphen;
    bool firstOfLine;
    std::string text;
  };
  std::vector<ListedWord> listed;
  std::vector<PageWords> pageWords;
  parserTextBlockHook = [](const TextBlock* block, const std::vector<std::string>& words, const std::vector<int16_t>& x,
                           const std::vector<uint8_t>& flags) { g_lines[block] = LineWords{words, x, flags}; };
  const auto onPage = [&](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t visible, uint32_t) {
    pages.push_back(visible);
    PageWords collected;
    collected.anchor = visible;
    uint16_t index = 0;
    static bool inStandIn = false;
    for (const auto& element : page->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto* line = static_cast<const PageLine*>(element.get());
      const auto found = g_lines.find(line->getBlock().get());
      if (found == g_lines.end()) continue;
      for (size_t i = 0; i < found->second.words.size(); i++) {
        const std::string& text = found->second.words[i];
        if (text.empty()) continue;
        // This host has no image decoders, so every image becomes "[Image: alt]". On a device
        // nearly all are pictures, with no words to select.
        if (text == "[Image:") inStandIn = true;
        if (inStandIn) {
          if (text.back() == ']') inStandIn = false;
          index++;
          continue;
        }
        const int x = line->xPos + (i < found->second.x.size() ? found->second.x[i] : 0);
        const bool hyphen =
            i < found->second.flags.size() && (found->second.flags[i] & TextBlock::WORD_FLAG_INSERTED_HYPHEN) != 0;
        collected.words.push_back({text, x, line->yPos, hyphen, index++});
        if (wantWords) listed.push_back({pages.size() - 1, hyphen, i == 0, text});
      }
    }
    pageWords.push_back(std::move(collected));
  };
  ChapterHtmlSlimParser parser{epub,
                               path,
                               renderer,
                               0,
                               1.0f,
                               false,
                               false,
                               0,
                               static_cast<uint16_t>(viewportWidth),
                               static_cast<uint16_t>(viewportHeight),
                               false,
                               false,
                               false,
                               0,
                               onPage,
                               true,
                               "",
                               "unused",
                               0,
                               {},
                               nullptr,
                               &css};
  const bool ok = parser.parseAndBuildPages();
  FILE* out = fopen(argv[2], "wb");
  if (!out) return 3;
  fprintf(out, "T\t%u\t%d\n", parser.getVisibleTextLength(), ok ? 1 : 0);
  for (size_t i = 0; i < pages.size(); i++) fprintf(out, "P\t%zu\t%u\n", i, pages[i]);
  if (const char* selections = getenv("XPPAGES_SELECTIONS")) writeSelections(out, pageWords, atoi(selections));
  for (const auto& w : listed) {
    fprintf(out, "W\t%zu\t%d\t%d\t%s\n", w.page, w.hyphen ? 1 : 0, w.firstOfLine ? 1 : 0, escape(w.text).c_str());
  }
  fclose(out);
  return 0;
}
