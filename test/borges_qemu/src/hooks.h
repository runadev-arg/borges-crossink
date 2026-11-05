#pragma once

#include <cstddef>
#include <cstdint>

constexpr size_t kBigAllocBytes = 2048;
constexpr size_t kBigAllocSlots = 64;

struct BigAlloc {
  uint32_t size;
  uint32_t freeBefore;
  uint32_t largestBefore;
  void* ra;
  bool ok;
};

extern volatile uint32_t gMaxRequest;
extern volatile uint32_t gFailedAllocs;
extern volatile uint32_t gLargestFailed;
extern BigAlloc gBig[kBigAllocSlots];
extern volatile uint32_t gBigCount;
extern volatile bool gTraceBig;
extern bool gSendMaxFragment;
