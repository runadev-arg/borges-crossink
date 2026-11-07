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
class Section;
struct CrossPointPosition;

namespace borges {

struct Position;
struct UnderlineBook;
struct UnderlineSegment;

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

// Before settings load, so they are there to load. One exists() per boot once it is done.
void importLegacySettingsAtBoot();
// The places in the books, with the panel ready: there can be hundreds. Normal boots only.
void importLegacyBooksAtBoot(GfxRenderer& renderer);
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
  bool totalPagesApproximate = false;  // This device's estimate, not the book's numbering.
};

// At reader exit, with the book and its clippings still loaded. Local, never the radio.
// True: the framebuffer was lent to resolve xpointers, so its bytes are gone. The caller
// holds the render lock, and rebuilds the frame if something after it needs one.
bool finalizeFromReader(GfxRenderer& renderer, ReaderTracker& tracker, const std::shared_ptr<Epub>& epub,
                        const ReaderSnapshot& snapshot);
// A restart that skips onExit is coming: what this reading knows goes to the card. For that restart
// only. Any path that runs onExit finalizes instead, and leaving a copy behind invents a session.
void keepReadingAcrossRestart(ReaderTracker& tracker, const Epub& epub);
// A place was just decided for this book: whatever was kept describes a reading that is over.
void discardKeptReading(const Epub& epub);
// At reader entry. Takes back what a restart or a failed finalize left, if it is recent enough.
void restoreReading(ReaderTracker& tracker, const Epub& epub);
// At reader entry, with the clippings loaded. The first time this account opens this book.
void seedClipsAtOpen(const Epub& epub);
// At reader entry. True once: the check for this book could not happen, and the page should say so.
bool takeReaderCheckNotice();
// Over the page just drawn. Stays until the page turns.
void drawCheckNotice(const GfxRenderer& renderer);
// A button took the screen off a session that is still busy. See armSession().
void noteLeftSessionOnPurpose();
// "Volver a donde estaba": the place restored becomes the head, as in the plugin.
void publishRestoredPosition(const std::shared_ptr<Epub>& epub, const Position& position);
// A clipping was just saved: remember when, next to the book.
void stampClip(const std::string& cachePath, uint16_t spine, uint32_t uptimeStamp,
               std::optional<uint32_t> visibleOffset);
void stampBookmark(const std::string& cachePath, uint16_t spine, float progress, std::optional<uint32_t> visibleOffset);

bool isPaired();
// Cheap enough to call when a menu opens: one exists() each.
bool hasUndo(const std::string& bookPath);
// False when there is none or it cannot be read. Stays until consumeUndo(), so that a place that
// could not be mapped back can be tried again.
bool peekUndo(const std::string& bookPath, Position& out);
void consumeUndo(const std::string& bookPath);

// Highlights of other readers. The answer is kept until the book or its clippings change: it may
// have to read every clipping on the first ask, and a menu opens often. Eight bytes of RAM.
bool hasInboundHighlights(const std::string& bookPath);
std::unique_ptr<Activity> makeInboundActivity(GfxRenderer& renderer, MappedInputManager& input,
                                              const std::string& bookPath);

// Other readers' highlights underlined in the page. Null: this book has none, and from here to the
// last page of it nothing else is asked or kept. One or two lookups, and what identifies the book only
// when the card has highlights of some book. See lib/BorgesCore/Underlines.h for what each step keeps.
std::unique_ptr<UnderlineBook> openUnderlines(const Epub& epub);
// A chapter that has one wants its whole layout now, while the reader waits for it anyway: where a
// highlight falls can only be said with every page known. Not for a chapter of megabytes.
bool underlinesWantWholeSection(const UnderlineBook& book, const Epub& epub, int spine);
// Entering a chapter. May lend the frame to walk it (the caller draws the page from scratch after
// this) and only when no saved plan is true for this log, layout and these clippings. A chapter still
// being laid out waits for the next time it is entered: a page turn is not when to do this.
void prepareUnderlines(UnderlineBook& book, GfxRenderer& renderer, const std::shared_ptr<Epub>& epub,
                       const Section& section, int spine, uint32_t layoutSignature);
// The segments of a page, into book.window. No file touched for a page without any, nor for one the
// last read already answered for. False: nothing to draw.
bool underlinesForPage(UnderlineBook& book, const Epub& epub, int spine, uint32_t layoutSignature, int page,
                       const UnderlineSegment*& segments, uint8_t& count);

enum class HighlightJumpOutcome : uint8_t { Exact, Approximate, Failed };
// Saves the place in the book and the way back, and says where this reader now is. The frame on the
// panel is lent for the mapping and comes back white: a toast can go on it either way. The caller
// holds the render lock. On success, the caller reopens the book, as after "Volver a donde estaba".
HighlightJumpOutcome jumpToHighlight(GfxRenderer& renderer, ReaderTracker& tracker, const std::shared_ptr<Epub>& epub,
                                     const CrossPointPosition& here, float herePercent, const std::string& xpointer,
                                     bool hasPct, uint32_t pctMicro);

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
