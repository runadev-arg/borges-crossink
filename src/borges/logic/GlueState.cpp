#include "GlueState.h"

#include <cstdio>
#include <cstring>

#include "AtomicFile.h"

namespace borges {
namespace glue {

namespace {

constexpr const char* kGlueBase = "/.crosspoint/borges/glue";
constexpr uint8_t kFormat = 3;
constexpr size_t kRecordBytes = 1 + 1 + 1 + 1 + 1 + 4 + 8 + 1 + 1 + 8;
constexpr size_t kRecordBytesV1 = kRecordBytes - 10;
constexpr size_t kStampBytes = 2 + 4 + 8 + 4;
constexpr uint8_t kTrackedFormat = 1;
constexpr size_t kTrackedBytes = 1 + 1 + 4 + 8 + 8 + 1;

uint64_t fnv64(const void* data, const size_t len, uint64_t hash = 14695981039346656037ULL) {
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    hash ^= p[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

void putU32(uint8_t* out, const uint32_t v) {
  for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t getU32(const uint8_t* in) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(in[i]) << (8 * i);
  return v;
}
void putI64(uint8_t* out, const int64_t v) {
  const auto u = static_cast<uint64_t>(v);
  for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(u >> (8 * i));
}
int64_t getI64(const uint8_t* in) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(in[i]) << (8 * i);
  return static_cast<int64_t>(v);
}

// Days since the epoch of a civil date (Howard Hinnant's algorithm).
int64_t daysFromCivil(int y, const unsigned m, const unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

}  // namespace

uint32_t pathHash(const char* path) {
  const uint64_t h = fnv64(path, path ? strlen(path) : 0);
  return static_cast<uint32_t>(h ^ (h >> 32));
}

bool armSession(GlueState& state, const PendingKind kind) {
  if (state.skipsLeft > 0) {
    --state.skipsLeft;
    state.pending = PendingKind::None;
    return false;
  }
  if (state.attempts >= kMaxAttempts) {
    state.attempts = 0;
    state.pending = PendingKind::None;
    if (state.cooldownLevel < kMaxCooldownLevel) ++state.cooldownLevel;
    state.skipsLeft = static_cast<uint8_t>((1u << state.cooldownLevel) - 1);
    return false;
  }
  ++state.attempts;
  state.pending = kind;
  return true;
}

void finishSession(GlueState& state) {
  state.attempts = 0;
  state.cooldownLevel = 0;
  state.skipsLeft = 0;
  state.pending = PendingKind::None;
}

void noteOpenSuccess(GlueState& state, const uint32_t bookPathHash, const WallTime& now) {
  if (now.precision == TimePrecision::Unknown) return;
  state.lastOpenPathHash = bookPathHash;
  state.lastOpenEpoch = now.epochSec;
}

bool openThrottled(const GlueState& state, const uint32_t bookPathHash, const WallTime& now) {
  if (now.precision == TimePrecision::Unknown || state.lastOpenEpoch <= 0) return false;
  if (state.lastOpenPathHash != bookPathHash) return false;
  const int64_t elapsed = now.epochSec - state.lastOpenEpoch;
  return elapsed >= 0 && elapsed < kOpenThrottleSec;
}

bool openOutlastsBackoff(const GlueState& state, const WallTime& now) {
  if (now.precision == TimePrecision::Unknown || state.lastOpenTryEpoch <= 0) return true;
  const int64_t elapsed = now.epochSec - state.lastOpenTryEpoch;
  return elapsed < 0 || elapsed >= kOpenBackoffCapSec;
}

OpenRoute routeOpen(const OpenInputs& in) {
  if (!in.isEpub || !in.paired) return OpenRoute::Direct;
  if (in.hasPendingJump || !in.autoOnOpen || in.needsRelink || in.backedOff || !in.hasSavedWifi || in.throttled ||
      in.batteryPercent < kMinSessionBatteryPercent) {
    return OpenRoute::DirectWithHold;
  }
  return OpenRoute::NetworkSession;
}

bool shouldFlushBeforeSleep(const SleepInputs& in) {
  return in.paired && in.syncOnSleep && in.hasSavedWifi && in.gateGo && !in.alreadyInNetworkBoot &&
         in.batteryPercent >= kMinSessionBatteryPercent;
}

GlueStore::GlueStore(IFs& fs) : fs_(fs) {}

bool GlueStore::load(GlueState& out) {
  out = GlueState{};
  AtomicFile file(fs_, kGlueBase);
  uint8_t buf[kRecordBytes];
  size_t len = 0;
  if (!file.load(buf, sizeof(buf), len)) return false;
  const bool v1 = buf[0] == 1 && len == kRecordBytesV1;
  if (!v1 && (buf[0] != kFormat || len != kRecordBytes)) return false;
  out.autoOnOpen = buf[1] != 0;
  out.syncOnSleep = buf[2] != 0;
  out.pending =
      buf[3] <= static_cast<uint8_t>(PendingKind::Manual) ? static_cast<PendingKind>(buf[3]) : PendingKind::None;
  out.attempts = buf[4];
  out.lastOpenPathHash = getU32(buf + 5);
  out.lastOpenEpoch = getI64(buf + 9);
  if (!v1) {
    out.cooldownLevel = buf[17];
    out.skipsLeft = buf[18];
    out.lastOpenTryEpoch = getI64(buf + 19);
  }
  return true;
}

bool GlueStore::save(const GlueState& state) {
  uint8_t buf[kRecordBytes];
  buf[0] = kFormat;
  buf[1] = state.autoOnOpen ? 1 : 0;
  buf[2] = state.syncOnSleep ? 1 : 0;
  buf[3] = static_cast<uint8_t>(state.pending);
  buf[4] = state.attempts;
  putU32(buf + 5, state.lastOpenPathHash);
  putI64(buf + 9, state.lastOpenEpoch);
  buf[17] = state.cooldownLevel;
  buf[18] = state.skipsLeft;
  putI64(buf + 19, state.lastOpenTryEpoch);
  fs_.mkdir("/.crosspoint");
  fs_.mkdir("/.crosspoint/borges");
  AtomicFile file(fs_, kGlueBase);
  return file.save(buf, sizeof(buf));
}

Age ageOf(const int64_t thenEpoch, const WallTime& now) {
  Age age;
  if (thenEpoch <= 0 || now.precision == TimePrecision::Unknown || now.epochSec < thenEpoch - 60) return age;
  const int64_t elapsed = now.epochSec > thenEpoch ? now.epochSec - thenEpoch : 0;
  if (elapsed < 60) {
    age.bucket = AgeBucket::JustNow;
  } else if (elapsed < 3600) {
    age.bucket = AgeBucket::Minutes;
    age.value = static_cast<int32_t>(elapsed / 60);
  } else if (elapsed < 86400) {
    age.bucket = AgeBucket::Hours;
    age.value = static_cast<int32_t>(elapsed / 3600);
  } else {
    age.bucket = AgeBucket::Days;
    age.value = static_cast<int32_t>(elapsed / 86400);
  }
  return age;
}

bool parseIsoEpoch(const char* text, int64_t& out) {
  if (!text) return false;
  int y = 0;
  unsigned mo = 0, d = 0, h = 0, mi = 0, s = 0;
  int consumed = 0;
  if (sscanf(text, "%4d-%2u-%2uT%2u:%2u:%2u%n", &y, &mo, &d, &h, &mi, &s, &consumed) != 6) return false;
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) return false;
  const char* tail = text + consumed;
  if (*tail == '.') {
    ++tail;
    while (*tail >= '0' && *tail <= '9') ++tail;
  }
  int64_t offset = 0;
  if (*tail == '+' || *tail == '-') {
    const int sign = *tail == '+' ? 1 : -1;
    unsigned oh = 0, om = 0;
    if (sscanf(tail + 1, "%2u:%2u", &oh, &om) != 2) return false;
    offset = sign * static_cast<int64_t>(oh * 3600 + om * 60);
  } else if (*tail != 'Z' && *tail != '\0') {
    return false;
  }
  out = daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s - offset;
  return true;
}

bool normalizeForSave(const int spine, const int page, const int pageCount, const int spineCount, SavedPosition& out) {
  if (spineCount <= 0 || spine < 0 || spine >= spineCount || spine > 0xFFFF) return false;
  const int safePage = page < 0 ? 0 : (page > 0xFFFE ? 0xFFFE : page);
  int count = pageCount;
  if (count < safePage + 1) count = safePage + 1;
  if (count > 0xFFFF) count = 0xFFFF;
  out.spine = spine;
  out.page = safePage;
  out.pageCount = count;
  return true;
}

bool planJump(const bool xpointerUsable, const char* xpointer, const bool hasPct, const uint32_t pctMicro,
              JumpRequest& out) {
  const bool useX = xpointerUsable && xpointer && xpointer[0] != '\0';
  if (!useX && !hasPct) return false;
  out.xpointer = useX ? xpointer : "";
  const uint32_t pct = pctMicro > 100000000U ? 100000000U : pctMicro;
  out.fraction = hasPct ? static_cast<float>(pct) / 100000000.0f : 0.0f;
  return true;
}

StaySource chooseStaySource(const bool hasFinalized, const Position& finalized, const Position& local) {
  if (!hasFinalized || !finalized.hasPct || finalized.xpointer[0] == '\0' || !local.hasPct) {
    return StaySource::MapSavedPage;
  }
  const uint32_t delta =
      finalized.pctMicro > local.pctMicro ? finalized.pctMicro - local.pctMicro : local.pctMicro - finalized.pctMicro;
  return delta <= kStayLedgerToleranceMicro ? StaySource::Ledger : StaySource::MapSavedPage;
}

bool saveTrackedReading(IFs& fs, const char* path, const TrackedReading& reading) {
  uint8_t rec[kTrackedBytes];
  rec[0] = kTrackedFormat;
  rec[1] = reading.paged ? 1 : 0;
  putU32(rec + 2, static_cast<uint32_t>(reading.pagesRead));
  putI64(rec + 6, reading.startedEpoch);
  putI64(rec + 14, reading.lastTurnEpoch);
  rec[22] = static_cast<uint8_t>(fnv64(rec, 22));
  File f(fs, path, IFs::Mode::WriteTrunc);
  return f && f.writeAll(rec, sizeof(rec)) && f.flush();
}

bool takeTrackedReading(IFs& fs, const char* path, TrackedReading& out) {
  if (!fs.exists(path)) return false;
  uint8_t rec[kTrackedBytes];
  bool whole = false;
  {
    File f(fs, path, IFs::Mode::Read);
    whole = f && f.readExact(rec, sizeof(rec)) && rec[0] == kTrackedFormat &&
            rec[22] == static_cast<uint8_t>(fnv64(rec, 22));
  }
  fs.remove(path);
  if (!whole) return false;
  out.paged = rec[1] != 0;
  out.pagesRead = static_cast<int32_t>(getU32(rec + 2));
  out.startedEpoch = getI64(rec + 6);
  out.lastTurnEpoch = getI64(rec + 14);
  return true;
}

bool ClipTimes::append(const char* path, const ClipStamp& stamp) {
  uint8_t rec[kStampBytes];
  rec[0] = static_cast<uint8_t>(stamp.spine);
  rec[1] = static_cast<uint8_t>(stamp.spine >> 8);
  putU32(rec + 2, stamp.uptime);
  putI64(rec + 6, stamp.epochMs);
  putU32(rec + 14, stamp.visibleOffset);
  File f(fs_, path, IFs::Mode::Append);
  return f && f.writeAll(rec, sizeof(rec)) && f.flush();
}

bool ClipTimes::lookup(const char* path, const uint16_t spine, const uint32_t uptime, ClipStamp& out) {
  File f(fs_, path, IFs::Mode::Read);
  if (!f) return false;
  bool found = false;
  uint8_t rec[kStampBytes];
  while (f.readExact(rec, sizeof(rec))) {
    const auto recSpine = static_cast<uint16_t>(rec[0] | (rec[1] << 8));
    if (recSpine == spine && getU32(rec + 2) == uptime) {  // The newest wins.
      out.spine = spine;
      out.uptime = uptime;
      out.epochMs = getI64(rec + 6);
      out.visibleOffset = getU32(rec + 14);
      found = true;
    }
  }
  return found;
}

uint64_t clipIdentity(const uint16_t startWord, const uint16_t textLength, const uint16_t wordCount) {
  uint64_t h = fnv64(&startWord, sizeof(startWord));
  h = fnv64(&textLength, sizeof(textLength), h);
  return fnv64(&wordCount, sizeof(wordCount), h);
}

uint64_t bookmarkIdentity(const uint32_t progressBits) { return fnv64(&progressBits, sizeof(progressBits)); }

}  // namespace glue
}  // namespace borges
