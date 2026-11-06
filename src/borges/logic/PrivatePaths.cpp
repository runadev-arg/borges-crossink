#include "PrivatePaths.h"

#include <cctype>
#include <cstddef>
#include <cstring>

namespace borges {
namespace glue {

namespace {

// Next path segment: skips separators and "." segments. False at the end.
bool nextSegment(const char*& p, const char*& start, size_t& len) {
  for (;;) {
    while (*p == '/' || *p == '\\') ++p;
    if (!*p) return false;
    start = p;
    while (*p && *p != '/' && *p != '\\') ++p;
    len = static_cast<size_t>(p - start);
    if (len != 1 || start[0] != '.') return true;
  }
}

bool sameName(const char* start, const size_t len, const char* name) {
  size_t i = 0;
  for (; i < len && name[i]; ++i) {
    if (tolower(static_cast<unsigned char>(start[i])) != name[i]) return false;
  }
  return i == len && !name[i];
}

bool startsWithName(const char* start, const size_t len, const char* prefix) {
  const size_t n = strlen(prefix);
  return len >= n && sameName(start, n, prefix);
}

}  // namespace

bool isBorgesPrivatePath(const char* path) {
  if (!path) return true;
  const char* p = path;
  const char* start = nullptr;
  size_t len = 0;
  if (!nextSegment(p, start, len)) return false;
  // Its short name is what FAT answers to as well: BORGES~1, CROSSP~1.
  if (sameName(start, len, ".borges") || startsWithName(start, len, "borges~")) return true;
  if (!sameName(start, len, ".crosspoint") && !startsWithName(start, len, "crossp~")) return false;
  return nextSegment(p, start, len) && (sameName(start, len, "borges") || startsWithName(start, len, "borges~"));
}

}  // namespace glue
}  // namespace borges
