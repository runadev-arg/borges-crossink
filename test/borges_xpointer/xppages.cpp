// Runs the firmware's real ChapterHtmlSlimParser and ParsedText over one spine item, with a
// fixed-width stand-in for the font, and writes what the section cache would keep:
//
//   xppages <spine.xhtml> <out> [css ...]
//   T <visible text length> <parse ok>
//   P <page> <visible offset of its first word>
//
// xpdiff reads this to hold the walker's count and each page start to real crengine.
#include <Epub.h>
#include <Epub/Page.h>
#include <GfxRenderer.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "Epub/css/CssParser.h"
#include "Epub/parsers/ChapterHtmlSlimParser.h"

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
  const auto onPage = [&](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t visible, uint32_t) {
    pages.push_back(visible);
  };
  ChapterHtmlSlimParser parser{epub,  path,  renderer, 0,      1.0f, false, false,    0, 480, 800,     false,
                               false, false, 0,        onPage, true, "",    "unused", 0, {},  nullptr, &css};
  const bool ok = parser.parseAndBuildPages();
  FILE* out = fopen(argv[2], "wb");
  if (!out) return 3;
  fprintf(out, "T\t%u\t%d\n", parser.getVisibleTextLength(), ok ? 1 : 0);
  for (size_t i = 0; i < pages.size(); i++) fprintf(out, "P\t%zu\t%u\n", i, pages[i]);
  fclose(out);
  return 0;
}
