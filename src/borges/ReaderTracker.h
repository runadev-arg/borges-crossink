#pragma once

// The only Borges state a reader holds while reading: a few integers, no heap, no IO.
// Page turns touch only this, so they cost nothing and never reach the card (rule 5).

#include <Arduino.h>

#include <cstdint>
#include <ctime>

namespace borges {
namespace glue {

struct ReaderTracker {
  bool paged = false;
  int32_t pagesRead = 0;
  int64_t startedEpoch = 0;
  int64_t lastTurnEpoch = 0;
  uint32_t lastTurnMs = 0;

  void onOpen() { startedEpoch = static_cast<int64_t>(time(nullptr)); }
  void onPageTurn(const bool forward) {
    paged = true;
    if (forward) ++pagesRead;
    lastTurnEpoch = static_cast<int64_t>(time(nullptr));
    lastTurnMs = millis();
  }
};

}  // namespace glue
}  // namespace borges
