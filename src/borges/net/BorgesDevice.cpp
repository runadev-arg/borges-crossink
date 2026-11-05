#include "BorgesDevice.h"

#if defined(SIMULATOR) || defined(BORGES_NET_PROBE)
#include <cstring>
#include <random>
#else
#include <esp_mac.h>
#include <esp_random.h>
#endif

namespace borges {
namespace net {

#if defined(SIMULATOR) || defined(BORGES_NET_PROBE)

void readChipMac(uint8_t out[6]) {
  const uint8_t fixed[6] = {0x02, 0x42, 0x4F, 0x52, 0x47, 0x53};
  memcpy(out, fixed, 6);
}

void EspRandom::fill(uint8_t* dst, const size_t len) {
  static std::random_device device;
  for (size_t i = 0; i < len; ++i) dst[i] = static_cast<uint8_t>(device());
}

#else

void readChipMac(uint8_t out[6]) { esp_efuse_mac_get_default(out); }

void EspRandom::fill(uint8_t* dst, const size_t len) { esp_fill_random(dst, len); }

#endif

DeadlineCancel::DeadlineCancel(ICancel& inner, IClock& clock, const uint32_t budgetMs)
    : inner_(inner), clock_(clock), deadlineMs_(clock.monotonicMs() + budgetMs) {}

bool DeadlineCancel::cancelled() { return inner_.cancelled() || clock_.monotonicMs() >= deadlineMs_; }

}  // namespace net
}  // namespace borges
