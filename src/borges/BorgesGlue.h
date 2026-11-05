#pragma once

// What the rest of the firmware calls. Each hook in an upstream file is one call into
// this header, so a CrossInk rebase only has to carry a handful of lines.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

class Activity;
class Epub;
class GfxRenderer;
class MappedInputManager;
struct CrossPointPosition;

namespace borges {

struct Position;

namespace glue {

struct ReaderTracker;

// The payload of NetworkBootTarget::BORGES. Persisted only across one reboot.
enum class NetPayload : uint32_t {
  None = 0,
  OpenBook = 1,          // APP_STATE.openEpubPath, then the reader.
  SleepFlush = 2,        // Headless, then deep sleep. The sleep image stays on the glass.
  Status = 3,            // The Borges screen, radio on demand.
  Pair = 4,              // The Borges screen, straight into pairing.
  ManualFromHome = 5,    // The Borges screen, straight into a sync.
  ManualFromReader = 6,  // A sync of the open book, then back to it.
  Library = 7,
  Disconnect = 8,   // The Borges screen, invalidating the credential on the server.
  OtherDevice = 9,  // APP_STATE.openEpubPath: the place another device left it at, then back.
};

// --- The previous firmware's data ------------------------------------------------------

// Before settings load. One exists() per boot once it is done.
void importLegacyDataAtBoot();
// Before the reader loads the book's bookmarks. Cheap once done, or when there were none.
void importLegacyBookmarks(Epub& epub);

// --- Opening a book -----------------------------------------------------------------

// The one door every user-initiated open goes through (ActivityManager::openBook).
// Returns the activity that takes over, or nullptr: open the book now. No reboot and no
// delay beyond reading the state when there is nothing to check.
std::unique_ptr<Activity> interceptOpen(GfxRenderer& renderer, MappedInputManager& input, const std::string& path);

// A wake into a book. True: this very boot runs the check, since it is already clean.
bool wakeWantsNetworkBoot(const std::string& path);
// A book entered by any door other than a session: its progress waits for a check.
void noteOpenedWithoutSession(const std::string& path);

// Called from main's setup() for NetworkBootTarget::BORGES. False: nothing could start.
bool launchNetworkTarget(uint32_t payload, GfxRenderer& renderer, MappedInputManager& input);

// --- Sleep ----------------------------------------------------------------------------

// Before deep sleep, after the sleep screen is on the panel and the reader has saved.
// True: reboot into the flush. Sleep then completes through finishDeepSleepAfterFlush().
bool shouldFlushBeforeSleep(uint32_t batteryPercent, bool inNetworkBoot);

// --- Reader ---------------------------------------------------------------------------

struct ReaderSnapshot {
  float percent = -1;                          // 0-100. Negative: unknown, no progress event.
  const CrossPointPosition* mapped = nullptr;  // For the xpointer. May be null.
  int32_t page = -1;                           // Stable reference pages. -1: unknown, omitted.
  int32_t totalPages = -1;
};

// At reader exit, with the book and its clippings still loaded. Local, never the radio.
// True: the framebuffer was lent to resolve xpointers, so its bytes are gone. The caller
// holds the render lock, and rebuilds the frame if something after it needs one.
bool finalizeFromReader(GfxRenderer& renderer, ReaderTracker& tracker, const std::shared_ptr<Epub>& epub,
                        const ReaderSnapshot& snapshot);
// A restart that skips onExit is coming: what this reading knows goes to the card.
void keepReadingAcrossRestart(const ReaderTracker& tracker, const Epub& epub);
// At reader entry. Takes back what a restart or a failed finalize left.
void restoreReading(ReaderTracker& tracker, const Epub& epub);
// "Volver a donde estaba": the place restored becomes the head, as in the plugin.
void publishRestoredPosition(const std::shared_ptr<Epub>& epub, const Position& position);
// A clipping was just saved: remember when, next to the book.
void stampClip(const std::string& cachePath, uint16_t spine, uint32_t uptimeStamp,
               std::optional<uint32_t> visibleOffset);
void stampBookmark(const std::string& cachePath, uint16_t spine, float progress, std::optional<uint32_t> visibleOffset);

bool isPaired();
// Cheap enough to call when a menu opens: one exists() each.
bool hasUndo(const std::string& bookPath);
// Single use. False when there is none or it cannot be read.
bool takeUndo(const std::string& bookPath, Position& out);

// The screen that turns a manual request into a network boot, after the current activity
// has run its onExit(). Replace the current activity with it.
std::unique_ptr<Activity> makeHandoff(GfxRenderer& renderer, MappedInputManager& input, NetPayload payload);
// Home and Settings: the status screen in the normal boot.
std::unique_ptr<Activity> makeStatusActivity(GfxRenderer& renderer, MappedInputManager& input);
// Button shortcut: sync the open book if there is one, else the Borges screen.
void startManualFromShortcut(GfxRenderer& renderer, MappedInputManager& input, bool inReader);

}  // namespace glue
}  // namespace borges

// In main.cpp, next to the rest of the deep sleep sequence.
void finishDeepSleepAfterBorgesFlush();
