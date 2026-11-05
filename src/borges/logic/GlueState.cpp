#include "GlueState.h"

#include <cstdio>
#include <cstring>

#include "AtomicFile.h"

namespace borges {
namespace glue {

namespace {

constexpr const char* kGlueBase = "/.crosspoint/borges/glue";
constexpr uint8_t kFormat = 1;
constexpr size_t kRecordBytes = 1 + 1 + 1 + 1 + 1 + 4 + 8;
constexpr size_t kStampBytes = 2 + 4 + 8;

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
  if (state.attempts >= kMaxAttempts) {
    state.attempts = 0;
    state.pending = PendingKind::None;
    return false;
  }
  ++state.attempts;
  state.pending = kind;
  return true;
}

void finishSession(GlueState& state) {
  state.attempts = 0;
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

OpenRoute routeOpen(const OpenInputs& in) {
  if (!in.isEpub || !in.paired) return OpenRoute::Direct;
  if (in.hasPendingJump || !in.autoOnOpen || in.needsRelink || in.backedOff || !in.hasSavedWifi || in.throttled) {
    return OpenRoute::DirectWithHold;
  }
  return OpenRoute::NetworkSession;
}

bool shouldFlushBeforeSleep(const SleepInputs& in) {
  return in.paired && in.syncOnSleep && in.hasSavedWifi && in.gateGo && !in.alreadyInNetworkBoot &&
         in.batteryPercent >= kMinSleepBatteryPercent;
}

GlueStore::GlueStore(IFs& fs) : fs_(fs) {}

bool GlueStore::load(GlueState& out) {
  out = GlueState{};
  AtomicFile file(fs_, kGlueBase);
  uint8_t buf[kRecordBytes];
  size_t len = 0;
  if (!file.load(buf, sizeof(buf), len) || len != kRecordBytes || buf[0] != kFormat) return false;
  out.autoOnOpen = buf[1] != 0;
  out.syncOnSleep = buf[2] != 0;
  out.pending =
      buf[3] <= static_cast<uint8_t>(PendingKind::Manual) ? static_cast<PendingKind>(buf[3]) : PendingKind::None;
  out.attempts = buf[4];
  out.lastOpenPathHash = getU32(buf + 5);
  out.lastOpenEpoch = getI64(buf + 9);
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

bool ClipTimes::append(const char* path, const ClipStamp& stamp) {
  uint8_t rec[kStampBytes];
  rec[0] = static_cast<uint8_t>(stamp.spine);
  rec[1] = static_cast<uint8_t>(stamp.spine >> 8);
  putU32(rec + 2, stamp.uptime);
  putI64(rec + 6, stamp.epochMs);
  File f(fs_, path, IFs::Mode::Append);
  return f && f.writeAll(rec, sizeof(rec)) && f.flush();
}

int64_t ClipTimes::lookup(const char* path, const uint16_t spine, const uint32_t uptime) {
  File f(fs_, path, IFs::Mode::Read);
  if (!f) return 0;
  int64_t found = 0;
  uint8_t rec[kStampBytes];
  while (f.readExact(rec, sizeof(rec))) {
    const auto recSpine = static_cast<uint16_t>(rec[0] | (rec[1] << 8));
    if (recSpine == spine && getU32(rec + 2) == uptime) found = getI64(rec + 6);  // The newest wins.
  }
  return found;
}

uint64_t clipIdentity(const uint32_t uptime, const uint16_t textLength, const uint16_t wordCount) {
  uint64_t h = fnv64(&uptime, sizeof(uptime));
  h = fnv64(&textLength, sizeof(textLength), h);
  return fnv64(&wordCount, sizeof(wordCount), h);
}

uint64_t bookmarkIdentity(const uint32_t progressBits) { return fnv64(&progressBits, sizeof(progressBits)); }

}  // namespace glue
}  // namespace borges
