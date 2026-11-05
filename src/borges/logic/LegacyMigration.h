#pragma once

// Brings over what the previous Borges firmware (a CrossPoint 1.6 build) kept in /.borges.
// That firmware renamed /.crosspoint to /.borges, so this one starts blind without this:
// no Wi-Fi, no settings, English, and every book back at page 1.
//
// Copies, never moves, and /.borges is never written: going back to the old firmware has to
// keep working, and it finds its folder exactly as it left it. Nothing here overwrites a
// file that already exists. A copy goes through a temporary name, so a power cut leaves
// either nothing or the whole file. The marker is written last: an interrupted run starts
// over, and finds its own work already done.
//
// Pure: no Arduino and no HAL, so test/borges_glue can run it natively.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Ports.h"

namespace borges {
namespace glue {

constexpr const char* kLegacyRoot = "/.borges";
constexpr const char* kCurrentRoot = "/.crosspoint";
constexpr const char* kLegacyImportMarker = "/.crosspoint/borges/legacy-import.done";
constexpr size_t kMaxLegacyBooks = 2048;

// IFs has no listing, and nothing else in the core needs one.
class IDirLister {
 public:
  virtual ~IDirLister() = default;
  // Names of the subdirectories of `dir`, at most `limit`. False if it cannot be read.
  virtual bool listDirectories(const char* dir, size_t limit, std::vector<std::string>& out) = 0;
};

enum class LegacyImport : uint8_t {
  NothingToDo,  // Already done, or there was never an old folder.
  Done,
  Incomplete,  // A write failed. Whatever was copied stays; the next boot goes on from there.
};

LegacyImport importLegacyData(IFs& fs, IDirLister& dirs);

// True if `to` holds the whole of `from` afterwards, or `from` does not exist. Never replaces `to`.
bool copyIfMissing(IFs& fs, const char* from, const char* to);

// The old firmware's bookmark file for a book: its path, flattened, under /.borges/bookmarks.
std::string legacyBookmarkPath(const std::string& bookPath);

// An old bookmark stored its place as a page of a page count. Now it is a fraction of the chapter.
float legacyBookmarkProgress(uint32_t pageCount, uint32_t page);

// "8-4-4-4-12" hex to 16 bytes. The id the server already knows that bookmark by.
bool parseUuid(const char* text, uint8_t out[16]);

}  // namespace glue
}  // namespace borges
