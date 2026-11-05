#pragma once
#include <cstdint>
#include <optional>
#include <string>
struct KOReaderRichPosition {
  uint32_t pctQ = 0;
  uint16_t spineIndex = 0;
  uint16_t pageNumber = 0;
  uint16_t totalPages = 1;
  std::optional<uint16_t> paragraphIndex;
  std::string xpath;
};
