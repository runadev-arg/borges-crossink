// Link-time hooks: the allocation trace, the optional max_fragment_length bypass, and
// pass-throughs for the --wrap flags inherited from [base] (the firmware defines them in
// lib/hal and src/platform, which this image does not link).
#include "hooks.h"

#include <esp_attr.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

volatile uint32_t gMaxRequest = 0;
volatile uint32_t gFailedAllocs = 0;
volatile uint32_t gLargestFailed = 0;
BigAlloc gBig[kBigAllocSlots];
volatile uint32_t gBigCount = 0;
volatile bool gTraceBig = false;
bool gSendMaxFragment = true;

namespace {
inline void pre(const size_t n, uint32_t& freeNow, uint32_t& largest) {
  freeNow = 0;
  largest = 0;
  if (n >= kBigAllocBytes && gTraceBig && !xPortInIsrContext()) {
    freeNow = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  }
}
inline void post(const size_t n, const void* p, const uint32_t freeNow, const uint32_t largest, void* ra) {
  if (n > gMaxRequest) gMaxRequest = n;
  if (p == nullptr && n != 0) {
    gFailedAllocs = gFailedAllocs + 1;
    if (n > gLargestFailed) gLargestFailed = n;
  }
  if (largest != 0 && gBigCount < kBigAllocSlots) {
    BigAlloc& b = gBig[gBigCount];
    b.size = n;
    b.freeBefore = freeNow;
    b.largestBefore = largest;
    b.ra = ra;
    b.ok = p != nullptr;
    gBigCount = gBigCount + 1;
  }
}
}  // namespace

#define TRACED(call, count)                       \
  uint32_t f, l;                                  \
  pre((count), f, l);                             \
  void* p = call;                                 \
  post((count), p, f, l, __builtin_return_address(0)); \
  return p

extern "C" {
void* __real_malloc(size_t);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
void* __real_heap_caps_malloc(size_t, uint32_t);
void* __real_heap_caps_calloc(size_t, size_t, uint32_t);

void* __wrap_malloc(size_t n) { TRACED(__real_malloc(n), n); }
void* __wrap_calloc(size_t a, size_t b) { TRACED(__real_calloc(a, b), a * b); }
void* __wrap_realloc(void* old, size_t n) { TRACED(__real_realloc(old, n), n); }
void* __wrap_heap_caps_malloc(size_t n, uint32_t caps) { TRACED(__real_heap_caps_malloc(n, caps), n); }
void* __wrap_heap_caps_calloc(size_t a, size_t b, uint32_t caps) {
  TRACED(__real_heap_caps_calloc(a, b, caps), a * b);
}

int __real_wolfSSL_UseMaxFragment(void* ssl, unsigned char mfl);
int __wrap_wolfSSL_UseMaxFragment(void* ssl, unsigned char mfl) {
  return gSendMaxFragment ? __real_wolfSSL_UseMaxFragment(ssl, mfl) : 1;
}

void __real_panic_print_backtrace(const void* frame, int core);
void __wrap_panic_print_backtrace(const void* frame, int core) { __real_panic_print_backtrace(frame, core); }
void __real_panic_abort(const char* details);
void __wrap_panic_abort(const char* details) { __real_panic_abort(details); }
// QEMU has no modem clock: the real ones assert. Only the Wi-Fi probe gets this far.
void __wrap_esp_phy_enable(int) {}
void __wrap_esp_phy_disable(int) {}
// The first thing the driver touches in the radio once it has started. Reports and returns.
void __wrap_phy_change_channel(int, int, int, int) {
  static int calls = 0;
  if (++calls <= 3) {
    esp_rom_printf("@@ wifi phy_change_channel reached: free=%u largest=%u\n",
                   static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                   static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
  }
}
esp_err_t __wrap_bootloader_common_check_efuse_blk_validity(uint32_t, uint32_t) { return ESP_OK; }
}
