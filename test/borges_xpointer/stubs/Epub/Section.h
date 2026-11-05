#pragma once
#include <memory>
#include <optional>
#include "Epub.h"
class GfxRenderer;
class Section {
 public:
  Section(const std::shared_ptr<Epub>&, int, GfxRenderer&) {}
  std::optional<int> getCachedPageCount() const { return std::nullopt; }
  std::optional<int> getPageForParagraphIndex(uint16_t) const { return std::nullopt; }
};
