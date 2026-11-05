#pragma once

// IClock on the system clock, with the label rule 6 demands. The label sits in
// RTC_NOINIT memory, so it survives esp_restart() (the network-boot bracket) and is
// lost with power. net/ClockRule.h decides it and says what IDF does with the clock
// across resets.

#include "Ports.h"

namespace borges {
namespace net {

class BorgesDns;

class BorgesClock : public IClock {
 public:
  // host: the server used for the Date fallbacks. Must outlive the clock.
  BorgesClock(BorgesDns& dns, const char* host);

  // Once per boot, before anything reads the clock. No network. storedAnchorEpoch is
  // store.state().anchorEpoch, 0 when there is none.
  void begin(int64_t storedAnchorEpoch);

  uint64_t monotonicMs() override;
  WallTime wallNow() override;
  void setWall(int64_t epochSec, TimePrecision precision) override;

  // After the link is up, before the first request. A valid clock returns at once;
  // otherwise anchor, then the Date and SNTP sources in the order the budget allows.
  // False means no source answered: TLS will report tls_clock.
  bool ensure(uint32_t budgetMs, ICancel& cancel);

  // A handshake failed on the certificate's dates with a clock that was not exact.
  // Ignores the anchor and asks the network. BorgesHttp calls it, once per session.
  bool repair(uint32_t budgetMs, ICancel& cancel);

 private:
  BorgesDns& dns_;
  const char* host_;
  int64_t anchorEpoch_ = 0;
};

}  // namespace net
}  // namespace borges
