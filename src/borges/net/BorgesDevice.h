#pragma once

// Small device facts the session glue needs.

#include <cstddef>
#include <cstdint>

#include "Ports.h"

namespace borges {
namespace net {

// The factory MAC from eFuse. Works with the radio off. Feed it to Store's key and to
// makeExternalId().
void readChipMac(uint8_t out[6]);

class EspRandom : public IRandom {
 public:
  void fill(uint8_t* dst, size_t len) override;
};

// Fires when the inner cancel does or when the deadline passes. Hand this to BorgesHttp
// and BorgesLink: a deadline only reaches a blocking read through ICancel.
class DeadlineCancel : public ICancel {
 public:
  DeadlineCancel(ICancel& inner, IClock& clock, uint32_t budgetMs);
  bool cancelled() override;

 private:
  ICancel& inner_;
  IClock& clock_;
  uint64_t deadlineMs_;
};

}  // namespace net
}  // namespace borges
