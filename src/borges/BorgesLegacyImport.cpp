// The device side of logic/LegacyMigration: listing the old folder, and the bookmarks, which
// need a book in hand because the old file names cannot be mapped back to a path.

#include <ArduinoJson.h>
#include <BookmarkStore.h>
#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <PersistableStore.h>

#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "BorgesGlue.h"
#include "BorgesStack.h"
#include "borges/logic/LegacyMigration.h"

namespace borges {
namespace glue {

namespace {

constexpr const char* kBookmarkMarker = "/borges_bookmarks_imported";
// A JSON document takes about twice its text. Beyond the free heap it waits for a later open.
constexpr size_t kBookmarkHeapFactor = 2;

class HalDirLister final : public IDirLister {
 public:
  bool listDirectories(const char* dir, const size_t limit, std::vector<std::string>& out) override {
    HalFile root = Storage.open(dir);
    if (!root || !root.isDirectory()) return false;
    char name[64];
    for (HalFile entry = root.openNextFile(); entry && out.size() < limit; entry = root.openNextFile()) {
      if (entry.isDirectory()) {
        entry.getName(name, sizeof(name));
        out.emplace_back(name);
      }
      entry.close();
    }
    root.close();
    return true;
  }
};

bool alreadyThere(const uint16_t spine, const float progress) {
  for (const Bookmark& mark : BOOKMARKS.getBookmarks()) {
    if (mark.spineIndex == spine && mark.progress == progress) return true;
  }
  return false;
}

// The server already holds these under their old ids. Told to the ledger, they are known
// bookmarks: no second copy on the account, and deleting one later names the right one.
void seedLedger(BorgesStack& stack, const BookId& id,
                const std::vector<std::pair<ClipKey, std::array<uint8_t, 16>>>& ids) {
  AnnotationRecord* stored = nullptr;
  uint16_t count = 0;
  if (!stack.store.loadAnnotations(id, &stored, &count)) return;
  const size_t room = count + ids.size();
  auto* next = new (std::nothrow) AnnotationRecord[room ? room : 1];
  if (next) {
    if (count) memcpy(next, stored, sizeof(AnnotationRecord) * count);
    size_t total = count;
    for (const auto& entry : ids) {
      if (total >= Store::kMaxAnnotations) break;
      next[total].key = events::clipLedgerKey(id, entry.first);
      memcpy(next[total].syncId, entry.second.data(), 16);
      next[total].bookmark = 1;
      ++total;
    }
    if (!stack.store.saveAnnotations(id, next, static_cast<uint16_t>(total))) {
      LOG_ERR("BORGES", "Could not record the imported bookmarks");
    }
    delete[] next;
  }
  delete[] stored;
}

}  // namespace

void importLegacyDataAtBoot() {
  net::BorgesFsHal fs;
  HalDirLister dirs;
  switch (importLegacyData(fs, dirs)) {
    case LegacyImport::NothingToDo:
      break;
    case LegacyImport::Done:
      LOG_INF("BORGES", "Imported the data of the previous firmware");
      break;
    case LegacyImport::Incomplete:
      LOG_ERR("BORGES", "Import of the previous firmware's data is incomplete: retrying at next boot");
      break;
  }
}

void importLegacyBookmarks(Epub& epub) {
  const std::string oldPath = legacyBookmarkPath(epub.getPath());
  if (!Storage.exists(oldPath.c_str())) return;
  const std::string marker = epub.getCachePath() + kBookmarkMarker;
  if (Storage.exists(marker.c_str())) return;

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

  std::vector<std::pair<ClipKey, std::array<uint8_t, 16>>> known;
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
    std::array<uint8_t, 16> syncId;
    const char* pending = obj["borges_event"] | "";
    if (pending[0] == '\0' && parseUuid(obj["borges_id"] | "", syncId.data())) {
      known.emplace_back(bookmarkKey(static_cast<uint16_t>(spine), UINT16_MAX, progress), syncId);
    }
  }
  BOOKMARKS.unload();

  if (!known.empty()) {
    auto stack = makeStack();
    BookId id;
    if (stack && stack->openStore() && stack->store.creds().paired() &&
        partialMd5(stack->fs, epub.getPath().c_str(), id)) {
      seedLedger(*stack, id, known);
    }
  }

  HalFile done;
  if (Storage.openFileForWrite("BORGES", marker, done)) done.close();
  LOG_INF("BORGES", "Imported old bookmarks of %s", epub.getPath().c_str());
}

}  // namespace glue
}  // namespace borges
