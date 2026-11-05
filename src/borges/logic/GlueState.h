#pragma once

// The firmware side of a Borges session that has to survive a reboot, and the decisions
// taken on it. Pure: no Arduino and no HAL, so test/borges_glue can run it natively.
//
// A session runs in its own boot (docs/borges-sync.md section 2). The boot token in RTC
// memory is single use, so a crash in that boot does not re-enter it. What the token
// cannot stop is the same crash on every open: the next open arms a new session. The
// attempt counter on SD bounds that. It is raised before the reboot and cleared when the
// session comes back, so a counter still up means the last session never came back.

#include <cstddef>
#include <cstdint>

#include "Ports.h"
#include "Types.h"

namespace borges {
namespace glue {

enum class PendingKind : uint8_t { None = 0, OpenBook = 1, SleepFlush = 2, Manual = 3 };

struct GlueState {
  bool autoOnOpen = true;
  bool syncOnSleep = true;
  PendingKind pending = PendingKind::None;
  uint8_t attempts = 0;
  uint32_t lastOpenPathHash = 0;  // Book of the last open-session that finished well.
  int64_t lastOpenEpoch = 0;
};

constexpr uint8_t kMaxAttempts = 2;
constexpr int64_t kOpenThrottleSec = 120;
constexpr uint32_t kMinSleepBatteryPercent = 15;

// Stable hash of a book's path. Not the edition: this one needs no read of the book.
uint32_t pathHash(const char* path);

// False: the last kMaxAttempts sessions never came back. The caller goes on without one,
// and the counter restarts, so a transient problem does not disable sync for good.
bool armSession(GlueState& state, PendingKind kind);
void finishSession(GlueState& state);

void noteOpenSuccess(GlueState& state, uint32_t bookPathHash, const WallTime& now);
// An untrustworthy or backwards clock never throttles: an extra check costs seconds, and
// a missed one could hide another device's position.
bool openThrottled(const GlueState& state, uint32_t bookPathHash, const WallTime& now);

enum class OpenRoute : uint8_t {
  Direct,          // Not paired, an excluded file: nothing to do, no reboot.
  DirectWithHold,  // No session now, but this book's progress waits for its next check.
  NetworkSession,  // Reboot into the check.
};

struct OpenInputs {
  bool isEpub = false;
  bool hasPendingJump = false;  // The reader chose a place on purpose (bookmark, clipping).
  bool paired = false;
  bool needsRelink = false;
  bool backedOff = false;
  bool hasSavedWifi = false;
  bool autoOnOpen = true;
  bool throttled = false;
};

OpenRoute routeOpen(const OpenInputs& in);

struct SleepInputs {
  bool paired = false;
  bool syncOnSleep = true;
  bool hasSavedWifi = false;
  bool gateGo = false;  // gate(Sleep): not relinking, not backed off, queue not empty.
  uint32_t batteryPercent = 0;
  bool alreadyInNetworkBoot = false;  // Sleeping out of a network boot: no second bracket.
};

bool shouldFlushBeforeSleep(const SleepInputs& in);

// A / B slots under /.crosspoint/borges. A missing or unreadable file reads as defaults.
class GlueStore {
 public:
  explicit GlueStore(IFs& fs);
  bool load(GlueState& out);
  bool save(const GlueState& state);

 private:
  IFs& fs_;
};

// Plain-words age of a past moment. Unknown when either clock cannot be trusted.
enum class AgeBucket : uint8_t { Unknown, JustNow, Minutes, Hours, Days };
struct Age {
  AgeBucket bucket = AgeBucket::Unknown;
  int32_t value = 0;
};
Age ageOf(int64_t thenEpoch, const WallTime& now);
// "2026-11-05T10:00:00Z" and friends. Offsets are honoured. False if it does not parse.
bool parseIsoEpoch(const char* text, int64_t& out);

// What a reader's saved page becomes once mapped onto this layout, in the shape the
// progress file wants: the page always inside its count, nothing out of the file's range.
struct SavedPosition {
  int spine = 0;
  int page = 0;
  int pageCount = 1;
};
bool normalizeForSave(int spine, int page, int pageCount, int spineCount, SavedPosition& out);

// Which position a prompt's "go there" asks the mapper for. Empty xpointer means the
// mapper falls back to the percentage. False: nothing usable, stay where the reader is.
struct JumpRequest {
  const char* xpointer = "";
  float fraction = 0;
};
bool planJump(bool xpointerUsable, const char* xpointer, bool hasPct, uint32_t pctMicro, JumpRequest& out);

// Where "stay here" takes its position from. The ledger already holds the xpointer of the
// last place this device published: if the book has not moved since, reading it costs no
// book load and keeps the radio up. Anything else has to be mapped from the saved page.
enum class StaySource : uint8_t { Ledger, MapSavedPage };
constexpr uint32_t kStayLedgerToleranceMicro = 1000000;  // 1%: a page, on a long chapter.
StaySource chooseStaySource(bool hasFinalized, const Position& finalized, const Position& local);

// Wall time of a clipping, kept next to the book so the clip file format stays as it is.
// A clip is identified by its chapter and the uptime stamp CrossInk already stores.
struct ClipStamp {
  uint16_t spine = 0;
  uint32_t uptime = 0;
  int64_t epochMs = 0;
};
class ClipTimes {
 public:
  explicit ClipTimes(IFs& fs) : fs_(fs) {}
  bool append(const char* path, const ClipStamp& stamp);
  // 0 when it has none: the event then omits the time, which is honest.
  int64_t lookup(const char* path, uint16_t spine, uint32_t uptime);

 private:
  IFs& fs_;
};

// CrossInk rewrites clip files and their text offsets. These fields do not change.
uint64_t clipIdentity(uint32_t uptime, uint16_t textLength, uint16_t wordCount);
uint64_t bookmarkIdentity(uint32_t uptime, uint32_t progressBits);

}  // namespace glue
}  // namespace borges
