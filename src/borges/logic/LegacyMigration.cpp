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

constexpr size_t kEpubProgressBytes = 6;  // Chapter, page, pages in it.

// Spine u16, page u16 and, in all but the oldest, page count u16 and a text offset u32.
bool readEpubProgress(IFs& fs, const char* path, uint8_t out[kEpubProgressBytes], size_t& len) {
  uint8_t raw[10];
  File f(fs, path, IFs::Mode::Read);
  if (!f) return false;
  const int32_t got = f.read(raw, sizeof(raw));
  if (got != 4 && got != 6 && got != 10) return false;
  const auto page = static_cast<uint32_t>(raw[2] | (raw[3] << 8));
  if (got >= 6) {
    const auto count = static_cast<uint32_t>(raw[4] | (raw[5] << 8));
    if (page != 0xFFFF && count > 0 && page >= count) return false;
  }
  len = static_cast<size_t>(got) < kEpubProgressBytes ? static_cast<size_t>(got) : kEpubProgressBytes;
  memcpy(out, raw, len);
  return true;
}

enum class BookCopy : uint8_t { Done, SourceBad, CannotWrite };

// An EPUB's place, without the text offset: it counted characters a way the old firmware's index
// did, and read against this one it lands somewhere else. The page is enough, and the first page
// turn writes a good offset. A main file that does not hold up loses to a backup that does.
BookCopy importEpubProgress(IFs& fs, const std::string& from, const std::string& backup, const std::string& to) {
  uint8_t place[kEpubProgressBytes];
  size_t len = 0;
  if (!readEpubProgress(fs, from.c_str(), place, len) && !readEpubProgress(fs, backup.c_str(), place, len)) {
    return BookCopy::SourceBad;
  }
  const std::string tmp = to + ".imp";
  {
    File target(fs, tmp.c_str(), IFs::Mode::WriteTrunc);
    if (!target || !target.writeAll(place, len) || !target.flush()) return BookCopy::CannotWrite;
  }
  return fs.rename(tmp.c_str(), to.c_str()) ? BookCopy::Done : BookCopy::CannotWrite;
}

// Only the place in the book. Layout caches are rebuilt, and the old format differs anyway.
// .bak: the old firmware kept the last whole position there across its two renames.
BookCopy importBookProgress(IFs& fs, const std::string& name) {
  const std::string from = std::string(kLegacyRoot) + "/" + name + "/progress.bin";
  const std::string backup = from + ".bak";
  const bool hasMain = fs.exists(from.c_str());
  if (!hasMain && !fs.exists(backup.c_str())) return BookCopy::Done;
  const std::string dir = std::string(kCurrentRoot) + "/" + name;
  if (!fs.mkdir(dir.c_str())) return BookCopy::CannotWrite;
  const std::string to = dir + "/progress.bin";
  if (fs.exists(to.c_str())) return BookCopy::Done;
  if (name.rfind("epub_", 0) == 0) return importEpubProgress(fs, from, backup, to);
  return copyIfMissing(fs, hasMain ? from.c_str() : backup.c_str(), to.c_str()) ? BookCopy::Done
                                                                                : BookCopy::CannotWrite;
}

class BookWalk final : public IDirVisitor {
 public:
  BookWalk(IFs& fs, ILegacyNotice* notice) : fs_(fs), notice_(notice) {}
  bool onDirectory(const char* name) override {
    if (!isBookCacheName(name)) return true;
    if (++seen_ == kLegacyBooksBeforeNotice && notice_) notice_->onManyBooks();
    switch (importBookProgress(fs_, name)) {
      case BookCopy::Done:
      case BookCopy::SourceBad:  // Nothing worth bringing. No later boot will read it better.
        return true;
      case BookCopy::CannotWrite:
        complete = false;
        return false;  // The card is full or failing: the rest would go the same way, one by one.
    }
    return true;
  }
  bool complete = true;

 private:
  IFs& fs_;
  ILegacyNotice* notice_;
  size_t seen_ = 0;
};

// Raised first: a boot that dies half way is one that counts.
bool countRun(IFs& fs, uint8_t& runs) {
  runs = 0;
  {
    File f(fs, kLegacyImportRuns, IFs::Mode::Read);
    if (f) f.readExact(&runs, 1);
  }
  if (runs >= kMaxLegacyImportRuns) return true;
  const uint8_t next = static_cast<uint8_t>(runs + 1);
  File f(fs, kLegacyImportRuns, IFs::Mode::WriteTrunc);
  return f && f.writeAll(&next, 1) && f.flush();
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

namespace {
bool copyTopLevelFiles(IFs& fs) {
  bool complete = true;
  for (const char* name : kTopLevelFiles) {
    const std::string from = std::string(kLegacyRoot) + "/" + name;
    const std::string to = std::string(kCurrentRoot) + "/" + name;
    if (!copyIfMissing(fs, from.c_str(), to.c_str())) complete = false;
  }
  return complete;
}
}  // namespace

void importLegacySettings(IFs& fs) {
  if (fs.exists(kLegacyImportMarker) || !fs.exists(kLegacyRoot) || !fs.mkdir(kCurrentRoot)) return;
  copyTopLevelFiles(fs);
}

LegacyImport importLegacyData(IFs& fs, IDirLister& dirs, ILegacyNotice* notice) {
  if (fs.exists(kLegacyImportMarker) || !fs.exists(kLegacyRoot)) return LegacyImport::NothingToDo;
  if (!fs.mkdir(kCurrentRoot) || !fs.mkdir("/.crosspoint/borges")) return LegacyImport::Incomplete;

  uint8_t runs = 0;
  // A card that cannot take one byte will not take a walk's worth, and the walk is the cost.
  if (!countRun(fs, runs)) return LegacyImport::Incomplete;
  if (runs >= kMaxLegacyImportRuns) return LegacyImport::GaveUp;

  bool complete = copyTopLevelFiles(fs);
  BookWalk walk(fs, notice);
  if (!dirs.forEachDirectory(kLegacyRoot, walk) || !walk.complete) complete = false;
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

std::string legacyBookmarkMarkerPath(const std::string& cachePath) {
  const size_t slash = cachePath.find_last_of('/');
  return std::string(kLegacyBookmarkMarkers) + "/" +
         (slash == std::string::npos ? cachePath : cachePath.substr(slash + 1));
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
