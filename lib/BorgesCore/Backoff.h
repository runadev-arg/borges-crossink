#pragma once

// Automatic sessions only (design rule 15). Manual always tries.

#include <cstdint>

#include "Ports.h"

namespace borges {

namespace backoff {

constexpr uint32_t kBaseSec = 15;
constexpr uint32_t kCapSec = 6 * 60 * 60;

// 15 * 2^failures seconds, capped at 6 h, with +-20% jitter. Never above the cap.
inline uint32_t delaySec(uint8_t failures, IRandom& random) {
  uint32_t base = kCapSec;
  if (failures < 16) {
    const uint64_t raw = static_cast<uint64_t>(kBaseSec) << failures;
    if (raw < kCapSec) base = static_cast<uint32_t>(raw);
  }
  uint8_t bytes[2];
  random.fill(bytes, sizeof(bytes));
  const uint32_t roll = (static_cast<uint32_t>(bytes[0]) << 8 | bytes[1]) % 401u;  // 0..400 tenths of a percent
  const uint64_t jittered = static_cast<uint64_t>(base) * (800u + roll) / 1000u;
  return jittered > kCapSec ? kCapSec : static_cast<uint32_t>(jittered);
}

// When the next automatic attempt may run. A Retry-After from the server only ever
// lengthens the wait. Zero means "no time gate", used when the wall clock is unknown:
// without a clock the safer failure is one extra attempt, not a device that never
// syncs again.
inline int64_t nextAttempt(const WallTime& now, uint32_t delay, bool hasRetryAfter, uint32_t retryAfter) {
  if (now.precision == TimePrecision::Unknown) return 0;
  if (hasRetryAfter && retryAfter > delay) delay = retryAfter;
  return now.epochSec + delay;
}

inline bool blocked(const WallTime& now, int64_t nextAttemptEpoch) {
  return nextAttemptEpoch != 0 && now.precision != TimePrecision::Unknown && now.epochSec < nextAttemptEpoch;
}

}  // namespace backoff

}  // namespace borges
