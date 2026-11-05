#pragma once

// The only Borges state a reader holds while reading: a few integers, no heap, no IO.
// Page turns touch only this, so they cost nothing and never reach the card (rule 5).

#include <Arduino.h>

#include <cstdint>
#include <ctime>

#include "PageStats.h"
#include "borges/logic/GlueState.h"

namespace borges {
namespace glue {

struct ReaderTracker : TrackedReading {
  static constexpr int64_t kPlausibleEpoch = 1600000000;

  uint32_t lastTurnMs = 0;  // 0: unknown, as after a restart.
  int32_t shownSpine = -1;
  int32_t shownPage = -1;
  int32_t startPage = -1;      // Reference page the reading began on.
  int32_t referencePage = -1;  // Reference page on the panel now.
  int64_t enteredEpoch = 0;    // When that page went up.
  PageStatBuffer pageStats;    // Heap-free: it lives in the activity, 800 bytes while reading.

  void onOpen() { startedEpoch = static_cast<int64_t>(time(nullptr)); }
  void onPageTurn(const bool forward) {
    if (forward) ++pagesRead;
    markMoved();
  }
  // Any way of getting to a page counts, as in the plugin: a TOC, a bookmark, a percent jump.
  // Called for each page put on the panel, so the first one only says where it started.
  void onPageShown(const int32_t spine, const int32_t page, const int32_t reference) {
    const bool moved = shownSpine >= 0 && (spine != shownSpine || page != shownPage);
    if (shownSpine >= 0 && !moved) return;
    leavePage();
    shownSpine = spine;
    shownPage = page;
    referencePage = reference;
    if (startPage < 0) startPage = reference;
    if (moved) markMoved();
  }
  // The page on the panel has not been left, but its time is over.
  void leavePage() {
    const int64_t now = static_cast<int64_t>(time(nullptr));
    if (referencePage >= 0 && now >= kPlausibleEpoch) pageStats.onLeave(referencePage, enteredEpoch, now);
    enteredEpoch = now >= kPlausibleEpoch ? now : 0;
  }

 private:
  void markMoved() {
    paged = true;
    lastTurnEpoch = static_cast<int64_t>(time(nullptr));
    lastTurnMs = millis();
  }
};

}  // namespace glue
}  // namespace borges
