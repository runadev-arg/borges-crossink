// The device side of logic/LegacyMigration: listing the old folder, and the bookmarks, which
// need a book in hand because the old file names cannot be mapped back to a path.

#include <ArduinoJson.h>
#include <BookmarkStore.h>
#include <Epub.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <PersistableStore.h>

#include <cstring>
#include <string>
#include <vector>

#include "BorgesGlue.h"
#include "BorgesStack.h"
#include "borges/logic/LegacyMigration.h"
#include "components/UITheme.h"

namespace borges {
namespace glue {

namespace {

constexpr const char* kOldBookmarkMarker = "/borges_bookmarks_imported";
// A JSON document takes about twice its text. Beyond the free heap it waits for a later open.
constexpr size_t kBookmarkHeapFactor = 2;

class HalDirLister final : public IDirLister {
 public:
  bool forEachDirectory(const char* dir, IDirVisitor& visitor) override {
    HalFile root = Storage.open(dir);
    if (!root || !root.isDirectory()) return false;
    char name[64];
    bool walking = true;
    for (HalFile entry = root.openNextFile(); entry && walking; entry = root.openNextFile()) {
      const bool directory = entry.isDirectory();
      if (directory) entry.getName(name, sizeof(name));
      entry.close();  // Before the visitor opens anything of its own.
      if (directory) walking = visitor.onDirectory(name);
    }
    root.close();
    return true;
  }
};

class PanelNotice final : public ILegacyNotice {
 public:
  explicit PanelNotice(GfxRenderer& renderer) : renderer_(renderer) {}
  void onManyBooks() override { GUI.drawPopup(renderer_, tr(STR_BORGES_ONE_MOMENT)); }

 private:
  GfxRenderer& renderer_;
};

bool alreadyThere(const uint16_t spine, const float progress) {
  for (const Bookmark& mark : BOOKMARKS.getBookmarks()) {
    if (mark.spineIndex == spine && mark.progress == progress) return true;
  }
  return false;
}

}  // namespace

void importLegacySettingsAtBoot() {
  net::BorgesFsHal fs;
  importLegacySettings(fs);
}

void importLegacyBooksAtBoot(GfxRenderer& renderer) {
  net::BorgesFsHal fs;
  HalDirLister dirs;
  PanelNotice notice(renderer);
  switch (importLegacyData(fs, dirs, &notice)) {
    case LegacyImport::NothingToDo:
      break;
    case LegacyImport::Done:
      LOG_INF("BORGES", "Imported the data of the previous firmware");
      break;
    case LegacyImport::Incomplete:
      LOG_ERR("BORGES", "Import of the previous firmware's data is incomplete: retrying at next boot");
      break;
    case LegacyImport::GaveUp:
      LOG_ERR("BORGES", "Import of the previous firmware's data gave up after too many boots");
      break;
  }
}

void importLegacyBookmarks(Epub& epub) {
  const std::string oldPath = legacyBookmarkPath(epub.getPath());
  if (!Storage.exists(oldPath.c_str())) return;
  const std::string marker = legacyBookmarkMarkerPath(epub.getCachePath());
  if (Storage.exists(marker.c_str())) return;
  // Done by a version that kept the marker in the cache. Carried over while it is still there.
  const bool carriedOver = Storage.exists((epub.getCachePath() + kOldBookmarkMarker).c_str());
  Storage.ensureDirectoryExists(kLegacyBookmarkMarkers);
  if (carriedOver) {
    HalFile carried;
    if (Storage.openFileForWrite("BORGES", marker, carried)) carried.close();
    return;
  }

  HalFile old;
  if (!Storage.openFileForRead("BORGES", oldPath, old)) return;
  const size_t oldSize = old.fileSize();
  old.close();
  if (oldSize * kBookmarkHeapFactor > ESP.getMaxAllocHeap()) {
    LOG_ERR("BORGES", "Old bookmarks do not fit in memory now (%u bytes)", static_cast<unsigned>(oldSize));
    return;
  }

  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(oldPath.c_str(), doc)) return;
  BOOKMARKS.loadForBook(epub.getPath(), epub.getTitle(), epub.getAuthor(), "epub");

  std::vector<KnownAnnotation> known;
  std::vector<ClipKey> keys;
  for (JsonObjectConst obj : doc["bookmarks"].as<JsonArrayConst>()) {
    const int spine = obj["si"] | -1;
    if (spine < 0 || spine >= epub.getSpineItemsCount()) continue;
    const float progress = legacyBookmarkProgress(obj["pc"] | 0U, obj["pp"] | 0U);
    if (!alreadyThere(static_cast<uint16_t>(spine), progress)) {
      const int toc = epub.getTocIndexForSpineIndex(spine);
      const std::string chapter = toc >= 0 ? epub.getTocItem(toc).title : std::string();
      BOOKMARKS.addBookmark(static_cast<uint16_t>(spine), progress, 0, chapter.c_str(), UINT16_MAX,
                            obj["summary"] | "");
    }
    // Still in the old queue: the server may never have seen it, so it goes out as new.
    KnownAnnotation entry;
    const char* pending = obj["borges_event"] | "";
    if (pending[0] == '\0' && parseUuid(obj["borges_id"] | "", entry.syncId)) {
      // The key can only be worked out with the book's id, a few lines below.
      entry.key = 0;
      keys.push_back(bookmarkKey(static_cast<uint16_t>(spine), UINT16_MAX, progress));
      known.push_back(entry);
    }
  }
  BOOKMARKS.unload();

  if (!known.empty()) {
    auto stack = makeStack();
    BookId id;
    if (stack && stack->openStore() && stack->store.creds().paired() &&
        partialMd5(stack->fs, epub.getPath().c_str(), id)) {
      // The server already holds these under their old ids. Told to the ledger, they are known
      // bookmarks: no second copy on the account, and deleting one later names the right one.
      for (size_t i = 0; i < known.size(); ++i) known[i].key = events::clipLedgerKey(id, keys[i]);
      if (!stack->store.addKnownAnnotations(id, known.data(), known.size(), true)) {
        LOG_ERR("BORGES", "Could not record the imported bookmarks");
      }
    }
  }

  HalFile done;
  if (Storage.openFileForWrite("BORGES", marker, done)) done.close();
  LOG_INF("BORGES", "Imported old bookmarks of %s", epub.getPath().c_str());
}

}  // namespace glue
}  // namespace borges
