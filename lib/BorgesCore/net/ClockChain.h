#pragma once

// Rule 6: which label a clock deserves, and which source to ask next.
//
// What ESP-IDF does with the clock (components/newlib/src/port/esp_time_impl.c, with
// CONFIG_LIBC_TIME_SYSCALL_USE_RTC_HRT=y and CONFIG_RTC_CLK_SRC_INT_RC=y in sdkconfig):
// gettimeofday() is the RTC timer plus a boot offset, and that offset lives in the
// RTC_BOOT_TIME_* store registers. Those registers and the RTC timer belong to the RTC
// domain, so:
//  - esp_restart(): the clock carries on. Same power cycle, a seconds-long gap on a
//    running timer. An exact clock stays exact, which is the network-boot bracket.
//  - Deep sleep: the clock also carries on, but timed by the 136 kHz internal RC, which
//    IDF documents as drifting by a few percent. Hours of sleep can be minutes off, so
//    it is not exact any more. It is anchored at best, and only if it is not behind the
//    stored anchor.
//  - Power-on reset: the RTC domain restarted and the clock reads near 1970. Unknown
//    until the anchor or the network says otherwise. HalGPIO and platformio.ini both
//    say an X4 wake can arrive as a power-on reset, so this may be the usual case on
//    this hardware; the rule does not depend on which.

#include <cstdint>

#include "Ports.h"

namespace borges {
namespace net {

enum class ResetKind : uint8_t { PowerOn, SoftwareRestart, DeepSleepWake, Crash };

// retained: the label kept in RTC memory, Unknown when none survived.
TimePrecision labelAtBoot(ResetKind reset, TimePrecision retained, int64_t rtcEpoch, int64_t storedAnchorEpoch);

enum class ClockSource : uint8_t { Sntp, HttpDate, HttpsDate };

// SNTP is exact. A Date header read off an unverified connection only proves that
// something answered, so it is approximate, until a verified server_time replaces it.
TimePrecision labelFor(ClockSource source);

struct ClockStep {
  ClockSource source;
  uint32_t maxMs;
};
struct ClockPlan {
  ClockStep steps[3];
};

// sdkconfig has CONFIG_LWIP_SNTP_STARTUP_DELAY=y with a 5000 ms maximum: lwIP waits a
// random time up to 5 s before its first SNTP packet, on top of DNS. On port 80 the same
// answer costs one TCP handshake and one round trip. So a short session asks the cheap
// sources first and leaves SNTP for last; a roomy one asks SNTP first, for its label.
constexpr uint32_t kRoomyClockBudgetMs = 30000;
constexpr uint32_t kSntpMaxMs = 10000;
ClockPlan clockPlanFor(uint32_t budgetMs);

class IClockSources {
 public:
  virtual ~IClockSources() = default;
  // True with an epoch. Each stops within budgetMs, and SNTP is stopped again on return,
  // so a late answer cannot move the clock afterwards.
  virtual bool read(ClockSource source, uint32_t budgetMs, ICancel& cancel, int64_t& epoch) = 0;
};

// A clock already labelled and in bounds is left alone, then the anchor, then the
// network. False when nothing answered.
bool ensureClock(IClock& clock, int64_t anchorEpoch, uint32_t budgetMs, IClockSources& sources, ICancel& cancel);
// The network only: the anchor is what just failed.
bool repairClock(IClock& clock, uint32_t budgetMs, IClockSources& sources, ICancel& cancel);

}  // namespace net
}  // namespace borges
