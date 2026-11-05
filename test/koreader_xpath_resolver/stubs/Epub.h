#pragma once

#include <Print.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// This test runs with no stylesheet; test/borges_xpointer feeds the real parser.
enum class CssDisplay : uint8_t { Block, None, Inline };
struct CssStyle {
  struct {
    bool whiteSpace = false;
    bool whiteSpacePreserved = false;
  } defined;
  CssDisplay display = CssDisplay::Block;
  bool hasDisplay() const { return false; }
};
class CssParser {
 public:
  bool empty() const { return true; }
  bool hasCache() const { return false; }
  bool loadFromCache() { return false; }
  void clear() {}
  CssStyle resolveStyle(const char*, const char*) const { return {}; }
};

class Epub {
 public:
  struct SpineItem {
    std::string href;
  };

  explicit Epub(std::vector<std::string> spineContents) : spineContents(std::move(spineContents)) {}

  CssParser* getCssParser() const { return nullptr; }

  int getSpineItemsCount() const { return static_cast<int>(spineContents.size()); }

  SpineItem getSpineItem(const int spineIndex) const {
    if (spineIndex < 0 || spineIndex >= getSpineItemsCount()) return {};
    return {"chapter" + std::to_string(spineIndex) + ".xhtml"};
  }

  bool readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize, bool = false) const {
    for (int spineIndex = 0; spineIndex < getSpineItemsCount(); spineIndex++) {
      if (itemHref != getSpineItem(spineIndex).href) continue;
      const auto& contents = spineContents[spineIndex];
      size_t offset = 0;
      while (offset < contents.size()) {
        const size_t size = std::min(chunkSize, contents.size() - offset);
        out.write(reinterpret_cast<const uint8_t*>(contents.data() + offset), size);
        offset += size;
      }
      return true;
    }
    return false;
  }

 private:
  std::vector<std::string> spineContents;
};
