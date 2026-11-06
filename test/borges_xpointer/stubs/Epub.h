#pragma once
#include <Epub/css/CssParser.h>
#include <Print.h>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
class GfxRenderer;
class Epub {
 public:
  struct SpineItem { std::string href; };
  struct SourceSpineMapEntry { int sourceSpineIndex = 0; uint8_t containerDepth = 0; size_t rangeCount = 0; };
  struct SourceChildRange { const char* name = ""; uint16_t count = 0; uint16_t offset = 0; };
  explicit Epub(std::vector<std::string> s) : spine(std::move(s)) {}
  int getSpineItemsCount() const { return (int)spine.size(); }
  SpineItem getSpineItem(int i) const {
    if (i < 0 || i >= getSpineItemsCount()) return {};
    return {"c" + std::to_string(i)};
  }
  static inline size_t bytesFed = 0;
  bool readItemContentsToStream(const std::string& h, Print& out, size_t chunk, bool allowEarlyStop = false) const {
    for (int i = 0; i < getSpineItemsCount(); i++) {
      if (h != getSpineItem(i).href) continue;
      auto& c = spine[i];
      size_t o = 0;
      while (o < c.size()) {
        size_t n = std::min(chunk, c.size() - o);
        bytesFed += n;
        // As ZipFile: a short write ends the stream, a success only if the caller allows it.
        if (out.write((const uint8_t*)c.data() + o, n) != n) return allowEarlyStop;
        o += n;
      }
      return true;
    }
    return false;
  }
  bool getItemSize(const std::string& h, size_t* s) const {
    for (int i = 0; i < getSpineItemsCount(); i++)
      if (h == getSpineItem(i).href) { *s = spine[i].size(); return true; }
    return false;
  }
  size_t getBookSize() const { size_t t = 0; for (auto& c : spine) t += c.size(); return t; }
  size_t getCumulativeSpineItemSize(int i) const {
    size_t t = 0;
    for (int k = 0; k <= i && k < getSpineItemsCount(); k++) t += spine[k].size();
    return t;
  }
  float calculateProgress(int i, float r) const {
    size_t p = i > 0 ? getCumulativeSpineItemSize(i - 1) : 0;
    return (p + r * spine[i].size()) / (float)getBookSize();
  }
  CssParser* getCssParser() const { return css.get(); }
  // The real firmware parser, fed the book's stylesheets.
  void loadCss(const std::vector<std::string>& sheets) {
    css = std::make_unique<CssParser>("");
    for (const auto& sheet : sheets) {
      HalFile file(sheet);
      css->loadFromStream(file);
    }
  }
  bool hasSourceSpineMap() const { return false; }
  bool requiresSourceSpineMap() const { return false; }
  bool getSourceSpineMapEntry(int, SourceSpineMapEntry&) const { return false; }
  const SourceChildRange* getSourceChildRange(const SourceSpineMapEntry&, size_t) const { return nullptr; }
  bool findCurrentSpineForSource(int, uint8_t, const char*, uint16_t, int&, uint16_t&) const { return false; }

 private:
  std::vector<std::string> spine;
  std::unique_ptr<CssParser> css;
};
