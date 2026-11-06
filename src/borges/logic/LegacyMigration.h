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
// Two steps, because the settings have to come before they are loaded and a card with hundreds of
// books has to walk them with something on the panel. Bounded: a card that cannot take the copies
// costs a few boots, not one walk per boot for ever.
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
constexpr const char* kLegacyImportRuns = "/.crosspoint/borges/legacy-import.runs";
constexpr const char* kLegacyBookmarkMarkers = "/.crosspoint/borges/bookmarks-imported";
constexpr uint8_t kMaxLegacyImportRuns = 5;
constexpr size_t kLegacyBooksBeforeNotice = 32;

class IDirVisitor {
 public:
  virtual ~IDirVisitor() = default;
  // False: stop walking.
  virtual bool onDirectory(const char* name) = 0;
};

// IFs has no listing, and nothing else in the core needs one. One name at a time: a list would be
// heap, and a cap on it would be books left behind by a run that believes it finished.
class IDirLister {
 public:
  virtual ~IDirLister() = default;
  // False if it cannot be read.
  virtual bool forEachDirectory(const char* dir, IDirVisitor& visitor) = 0;
};

// A long walk is under way: time to say so on the panel. Called once, at most.
class ILegacyNotice {
 public:
  virtual ~ILegacyNotice() = default;
  virtual void onManyBooks() = 0;
};

enum class LegacyImport : uint8_t {
  NothingToDo,  // Already done, or there was never an old folder.
  Done,
  Incomplete,  // A write failed. Whatever was copied stays; the next boot goes on from there.
  GaveUp,      // It failed too many boots in a row: what came over stays, and it stops trying.
};

// Before settings load. What it cannot copy now is tried again by importLegacyData.
void importLegacySettings(IFs& fs);
LegacyImport importLegacyData(IFs& fs, IDirLister& dirs, ILegacyNotice* notice = nullptr);

// True if `to` holds the whole of `from` afterwards, or `from` does not exist. Never replaces `to`.
bool copyIfMissing(IFs& fs, const char* from, const char* to);

// The old firmware's bookmark file for a book: its path, flattened, under /.borges/bookmarks.
std::string legacyBookmarkPath(const std::string& bookPath);

// Says this book's old bookmarks came over. Not next to the book: clearing its cache must not
// bring back the ones deleted since.
std::string legacyBookmarkMarkerPath(const std::string& cachePath);

// An old bookmark stored its place as a page of a page count. Now it is a fraction of the chapter.
float legacyBookmarkProgress(uint32_t pageCount, uint32_t page);

// "8-4-4-4-12" hex to 16 bytes. The id the server already knows that bookmark by.
bool parseUuid(const char* text, uint8_t out[16]);

}  // namespace glue
}  // namespace borges
