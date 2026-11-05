#pragma once

#include <cstring>

// The visible text offset is the coordinate a page position is stored in, and the KOReader
// position mapper has to count it from a bare stream of the same XHTML with no CSS and no
// layout. So it counts every character the XML parser reports, whether it ends up on a page
// or not (display:none, nested tables, unsupported elements), except inside these elements.
inline bool isVisibleOffsetExcludedTag(const char* name) {
  static constexpr const char* const kExcluded[] = {"head", "style", "script", "title", "rp", "rt"};
  for (const char* excluded : kExcluded) {
    if (strcmp(name, excluded) == 0) return true;
  }
  return false;
}
