#include "LegacyMigration.h"

#include <algorithm>
#include <cstring>

namespace borges {
namespace glue {

namespace {

// settings.json goes through CrossPointSettings' own legacy path, which rewrites it in the
// current format. The others are read as they are.
constexpr const char* kTopLevelFiles[] = {"settings.json", "wifi.json", "recent.json", "opds.json", "koreader.json"};
constexpr size_t kCopyChunk = 256;

bool isBookCacheName(const std::string& name) {
  return name.rfind("epub_", 0) == 0 || name.rfind("xtc_", 0) == 0 || name.rfind("txt_", 0) == 0;
}

// Only the place in the book. Layout caches are rebuilt, and the old format differs anyway.
// .bak: the old firmware kept the last whole position there across its two renames.
bool importBookProgress(IFs& fs, const std::string& name) {
  const std::string from = std::string(kLegacyRoot) + "/" + name + "/progress.bin";
  const std::string backup = from + ".bak";
  const bool hasMain = fs.exists(from.c_str());
  if (!hasMain && !fs.exists(backup.c_str())) return true;
  const std::string dir = std::string(kCurrentRoot) + "/" + name;
  if (!fs.mkdir(dir.c_str())) return false;
  const std::string to = dir + "/progress.bin";
  return copyIfMissing(fs, hasMain ? from.c_str() : backup.c_str(), to.c_str());
}

int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

bool copyIfMissing(IFs& fs, const char* from, const char* to) {
  if (fs.exists(to) || !fs.exists(from)) return true;
  const std::string tmp = std::string(to) + ".imp";
  {
    File source(fs, from, IFs::Mode::Read);
    File target(fs, tmp.c_str(), IFs::Mode::WriteTrunc);
    if (!source || !target) return false;
    uint8_t chunk[kCopyChunk];
    for (;;) {
      const int32_t got = source.read(chunk, sizeof(chunk));
      if (got < 0) return false;
      if (got == 0) break;
      if (!target.writeAll(chunk, static_cast<size_t>(got))) return false;
    }
    if (!target.flush()) return false;
  }
  return fs.rename(tmp.c_str(), to);
}

LegacyImport importLegacyData(IFs& fs, IDirLister& dirs) {
  if (fs.exists(kLegacyImportMarker) || !fs.exists(kLegacyRoot)) return LegacyImport::NothingToDo;
  if (!fs.mkdir(kCurrentRoot) || !fs.mkdir("/.crosspoint/borges")) return LegacyImport::Incomplete;

  bool complete = true;
  for (const char* name : kTopLevelFiles) {
    const std::string from = std::string(kLegacyRoot) + "/" + name;
    const std::string to = std::string(kCurrentRoot) + "/" + name;
    if (!copyIfMissing(fs, from.c_str(), to.c_str())) complete = false;
  }

  std::vector<std::string> books;
  if (!dirs.listDirectories(kLegacyRoot, kMaxLegacyBooks, books)) {
    complete = false;
  } else {
    for (const std::string& name : books) {
      if (isBookCacheName(name) && !importBookProgress(fs, name)) complete = false;
    }
  }
  if (!complete) return LegacyImport::Incomplete;

  File marker(fs, kLegacyImportMarker, IFs::Mode::WriteTrunc);
  if (!marker || !marker.flush()) return LegacyImport::Incomplete;
  return LegacyImport::Done;
}

std::string legacyBookmarkPath(const std::string& bookPath) {
  std::string name = bookPath.empty() ? bookPath : bookPath.substr(1);
  std::replace(name.begin(), name.end(), '/', '_');
  std::replace(name.begin(), name.end(), '\\', '_');
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos) name.erase(dot);
  return std::string(kLegacyRoot) + "/bookmarks/" + name + ".json";
}

float legacyBookmarkProgress(const uint32_t pageCount, const uint32_t page) {
  if (pageCount == 0) return 0.0f;
  const float progress = static_cast<float>(page) / static_cast<float>(pageCount);
  return progress > 1.0f ? 1.0f : progress;
}

bool parseUuid(const char* text, uint8_t out[16]) {
  if (!text || strlen(text) != 36) return false;
  size_t written = 0;
  for (size_t i = 0; i < 36;) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (text[i] != '-') return false;
      ++i;
      continue;
    }
    const int high = hexValue(text[i]);
    const int low = hexValue(text[i + 1]);
    if (high < 0 || low < 0 || written >= 16) return false;
    out[written++] = static_cast<uint8_t>((high << 4) | low);
    i += 2;
  }
  return written == 16;
}

}  // namespace glue
}  // namespace borges
