#include "net/ClockChain.h"

#include <cstring>

#include "Codec.h"
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

bool repairAcceptable(const int64_t epoch, const ClockTrust& trust) {
  if (!epochInBounds(epoch) || epoch < trust.buildEpoch || epoch < trust.anchorEpoch) return false;
  if (trust.allowLongGap) return true;
  // A firmware newer than the anchor is itself proof that time has passed.
  const int64_t known = trust.anchorEpoch > trust.buildEpoch ? trust.anchorEpoch : trust.buildEpoch;
  return known == 0 || epoch - known <= kMaxClockGapSec;
}

int64_t epochOfBuildDate(const char* date) {
  static constexpr char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  if (!date || strlen(date) != 11) return 0;
  const char* found = nullptr;
  for (const char* m = kMonths; *m && !found; m += 3) {
    if (memcmp(m, date, 3) == 0) found = m;
  }
  if (!found) return 0;
  const int month = static_cast<int>((found - kMonths) / 3) + 1;
  const int day = (date[4] == ' ' ? 0 : date[4] - '0') * 10 + (date[5] - '0');
  char text[21] = "0000-00-00T00:00:00Z";
  memcpy(text, date + 7, 4);
  text[5] = static_cast<char>('0' + month / 10);
  text[6] = static_cast<char>('0' + month % 10);
  text[8] = static_cast<char>('0' + day / 10);
  text[9] = static_cast<char>('0' + day % 10);
  int64_t epoch = 0;
  return parseRfc3339(text, epoch) ? epoch : 0;
}

ClockPlan clockPlanFor(const uint32_t budgetMs) {
  if (budgetMs >= kRoomyClockBudgetMs) {
    return ClockPlan{{{ClockSource::Sntp, kSntpMaxMs}, {ClockSource::HttpDate, 4000}, {ClockSource::HttpsDate, 10000}}};
  }
  return ClockPlan{{{ClockSource::HttpDate, 2500}, {ClockSource::HttpsDate, 5000}, {ClockSource::Sntp, kSntpMaxMs}}};
}

bool repairClock(IClock& clock, const ClockTrust& trust, const uint32_t budgetMs, IClockSources& sources, ICancel& cancel) {
  const uint64_t deadline = clock.monotonicMs() + budgetMs;
  const ClockPlan plan = clockPlanFor(budgetMs);
  for (const ClockStep& step : plan.steps) {
    if (cancel.cancelled()) return false;
    const uint64_t now = clock.monotonicMs();
    if (now >= deadline) return false;
    const uint32_t left = static_cast<uint32_t>(deadline - now);
    int64_t epoch = 0;
    if (!sources.read(step.source, left < step.maxMs ? left : step.maxMs, cancel, epoch)) continue;
    if (!repairAcceptable(epoch, trust)) continue;
    clock.setWall(epoch, TimePrecision::Approximate);
    return true;
  }
  return false;
}

bool ensureClock(IClock& clock, const ClockTrust& trust, const uint32_t budgetMs, IClockSources& sources,
                 ICancel& cancel) {
  const WallTime now = clock.wallNow();
  if (now.precision != TimePrecision::Unknown && epochInBounds(now.epochSec)) return true;
  if (epochInBounds(trust.anchorEpoch)) {
    clock.setWall(trust.anchorEpoch, TimePrecision::Anchored);
    return true;
  }
  return repairClock(clock, trust, budgetMs, sources, cancel);
}

}  // namespace net
}  // namespace borges
