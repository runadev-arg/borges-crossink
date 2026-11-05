#include "ResumePolicy.h"

#include <cstring>

namespace borges {

namespace resume {

uint64_t fingerprint(const RemoteEvent& e) {
  if (e.hasEventId) {
    uint64_t h = fnv1a64("event:", 6);
    return fnv1a64(e.eventId, strlen(e.eventId), h);
  }
  if (e.hasSeq) {
    uint8_t raw[8];
    ByteWriter w(raw, sizeof(raw));
    w.u64(e.serverSeq);
    return fnv1a64(raw, sizeof(raw), fnv1a64("seq:", 4));
  }
  uint64_t h = fnv1a64("pos:", 4);
  h = fnv1a64(e.occurredAt, strlen(e.occurredAt), h);
  h = fnv1a64(e.originId, strlen(e.originId), h);
  h = fnv1a64(e.xpointer, strlen(e.xpointer), h);
  uint8_t raw[9];
  ByteWriter w(raw, sizeof(raw));
  w.u32(static_cast<uint32_t>(e.page));
  w.u8(e.hasPct ? 1 : 0);
  w.u32(e.pctMicro);
  return fnv1a64(raw, sizeof(raw), h);
}

Verdict evaluate(const RemoteEvent* e, const BookId& book, const char* selfDeviceId, const BookLedger& ledger) {
  if (!e) return Verdict::NoEvent;
  const bool sameBook = strcmp(e->bookKind, "koreader_partial_md5") == 0 && strcmp(e->bookValue, book.hex) == 0;
  // Nothing to navigate to is as useless as no event, and worse if it were offered.
  const bool hasLocator = e->xpointerUsable || e->hasPct;
  if (!sameBook || !e->hasEventId || !e->originId[0] || !e->readingHead || !hasLocator) return Verdict::Invalid;
  if (selfDeviceId && strcmp(e->originId, selfDeviceId) == 0) return Verdict::Own;
  // An answer stands for that event, whatever was read here afterwards, as in
  // resumeflow.lua:hasLocalChoice. Asking again would offer Go over pages read since.
  return ledger.hasChoice(fingerprint(*e)) ? Verdict::AlreadyChosen : Verdict::Ask;
}

uint8_t warningsFor(bool hasRemotePct, uint32_t remotePct, bool hasLocalPct, uint32_t localPct, bool xpointerUsable) {
  uint8_t mask = 0;
  if (hasRemotePct && hasLocalPct) {
    const int64_t delta = static_cast<int64_t>(remotePct) - static_cast<int64_t>(localPct);
    if (delta < kBackwardsMicro) {
      mask |= kWarnBackwards;
    } else if (delta <= kCloseMicro && delta >= -kCloseMicro) {
      mask |= kWarnClose;
    }
  }
  if (!xpointerUsable) mask |= kWarnApproximate;
  return mask;
}

size_t warningCopy(uint8_t mask, const char* out[3]) {
  size_t n = 0;
  if (mask & kWarnBackwards) out[n++] = kCopyWarnBackwards;
  if (mask & kWarnClose) out[n++] = kCopyWarnClose;
  if (mask & kWarnApproximate) out[n++] = kCopyWarnApproximate;
  return n;
}

}  // namespace resume

}  // namespace borges
