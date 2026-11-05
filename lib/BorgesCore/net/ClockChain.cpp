#include "net/ClockChain.h"

#include "ResumePolicy.h"
#include "net/HttpDate.h"

namespace borges {
namespace net {

TimePrecision labelAtBoot(const ResetKind reset, const TimePrecision retained, const int64_t rtcEpoch,
                          const int64_t storedAnchorEpoch) {
  if (!epochInBounds(rtcEpoch)) return TimePrecision::Unknown;
  if (reset == ResetKind::SoftwareRestart) {
    if (retained == TimePrecision::Exact) return clockpolicy::labelAfterBoot(true, rtcEpoch, storedAnchorEpoch);
    if (retained == TimePrecision::Approximate) return TimePrecision::Approximate;
  }
  return clockpolicy::labelAfterBoot(false, rtcEpoch, storedAnchorEpoch);
}

TimePrecision labelFor(const ClockSource source) {
  return source == ClockSource::Sntp ? TimePrecision::Exact : TimePrecision::Approximate;
}

ClockPlan clockPlanFor(const uint32_t budgetMs) {
  if (budgetMs >= kRoomyClockBudgetMs) {
    return ClockPlan{{{ClockSource::Sntp, kSntpMaxMs}, {ClockSource::HttpDate, 4000}, {ClockSource::HttpsDate, 10000}}};
  }
  return ClockPlan{{{ClockSource::HttpDate, 2500}, {ClockSource::HttpsDate, 5000}, {ClockSource::Sntp, kSntpMaxMs}}};
}

bool repairClock(IClock& clock, const uint32_t budgetMs, IClockSources& sources, ICancel& cancel) {
  const uint64_t deadline = clock.monotonicMs() + budgetMs;
  const ClockPlan plan = clockPlanFor(budgetMs);
  for (const ClockStep& step : plan.steps) {
    if (cancel.cancelled()) return false;
    const uint64_t now = clock.monotonicMs();
    if (now >= deadline) return false;
    const uint32_t left = static_cast<uint32_t>(deadline - now);
    int64_t epoch = 0;
    if (!sources.read(step.source, left < step.maxMs ? left : step.maxMs, cancel, epoch)) continue;
    if (!epochInBounds(epoch)) continue;
    clock.setWall(epoch, labelFor(step.source));
    return true;
  }
  return false;
}

bool ensureClock(IClock& clock, const int64_t anchorEpoch, const uint32_t budgetMs, IClockSources& sources,
                 ICancel& cancel) {
  const WallTime now = clock.wallNow();
  if (now.precision != TimePrecision::Unknown && epochInBounds(now.epochSec)) return true;
  if (epochInBounds(anchorEpoch)) {
    clock.setWall(anchorEpoch, TimePrecision::Anchored);
    return true;
  }
  return repairClock(clock, budgetMs, sources, cancel);
}

}  // namespace net
}  // namespace borges
