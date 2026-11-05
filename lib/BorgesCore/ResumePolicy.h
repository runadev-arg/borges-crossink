#pragma once

// The KOReader decision table for "another device read this book", pure. Copy of the
// behaviour in borges.koplugin/resumeflow.lua and main.lua:_handleReadingHead.

#include <cstddef>
#include <cstdint>

#include "Events.h"
#include "Store.h"

namespace borges {

namespace resume {

enum class Verdict : uint8_t {
  NoEvent,        // focus_event is null: nobody read this edition.
  Own,            // This device is the head.
  AlreadyChosen,  // Same event answered before.
  Invalid,        // Reported as an error. Never a prompt.
  Ask,            // Prompt, even at the same spot.
};

// Warning bits. Backwards and Close exclude each other, as in KOReader.
constexpr uint8_t kWarnBackwards = 1 << 0;
constexpr uint8_t kWarnClose = 1 << 1;
constexpr uint8_t kWarnApproximate = 1 << 2;

constexpr int64_t kBackwardsMicro = -500000;  // Delta below -0.5%.
constexpr int64_t kCloseMicro = 2000000;      // |Delta| at most 2%.

// Stable identity of a proposed position: event:<id>, else seq:<n>, else the position.
// Stored as a hash so forty of them fit in the ledger.
uint64_t fingerprint(const RemoteEvent& event);

Verdict evaluate(const RemoteEvent* focusEvent, const BookId& book, const char* selfDeviceId, const BookLedger& ledger);

uint8_t warningsFor(bool hasRemotePct, uint32_t remotePct, bool hasLocalPct, uint32_t localPct, bool xpointerUsable);

// Dates are shown only for clocks worth believing.
inline bool showsDate(TimePrecision p) { return p == TimePrecision::Exact || p == TimePrecision::Anchored; }

// UI copy, in Spanish, UTF-8.
constexpr const char* kCopyTitle = "\xC2\xBF" "D\xC3\xB3" "nde quer\xC3\xA9" "s seguir leyendo?";
constexpr const char* kCopyGoThere = "Ir a esa posici\xC3\xB3n";
constexpr const char* kCopyStayHere = "Seguir aqu\xC3\xAD";
constexpr const char* kCopyHereLabel = "En este lector";
constexpr const char* kCopyUndo = "Volver a donde estaba";
constexpr const char* kCopyWarnBackwards = "Ir ah\xC3\xAD te lleva hacia atr\xC3\xA1s en el libro.";
constexpr const char* kCopyWarnClose = "La diferencia entre las dos posiciones es chica.";
constexpr const char* kCopyWarnApproximate =
    "La ubicaci\xC3\xB3n es aproximada: puede caer unos renglones antes o despu\xC3\xA9s.";

// Fills out with the sentences for a warning mask, in display order. Returns the count.
size_t warningCopy(uint8_t mask, const char* out[3]);

}  // namespace resume

// Clock labelling (design rule 6), pure.
namespace clockpolicy {

// server_time is applied only when the clock is invalid or worse than that anchor.
// A fresh server_time counts as exact, so an exact clock is left alone.
inline bool shouldApplyServerTime(const WallTime& current) { return current.precision != TimePrecision::Exact; }

// The label for the system clock after a boot. exact needs SNTP or HTTPS this power
// cycle; a clock that survived deep sleep and is not behind the stored anchor is
// anchored; anything else is unknown, whatever the clock happens to say.
inline TimePrecision labelAfterBoot(bool syncedThisPowerCycle, int64_t rtcEpoch, int64_t storedAnchorEpoch) {
  if (syncedThisPowerCycle) return TimePrecision::Exact;
  if (storedAnchorEpoch > 0 && rtcEpoch >= storedAnchorEpoch) return TimePrecision::Anchored;
  return TimePrecision::Unknown;
}

}  // namespace clockpolicy

}  // namespace borges
