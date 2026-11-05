#include "Positions.h"

#include <cstring>

namespace borges {

namespace {

TimePrecision precisionOf(const char* text) {
  if (strcmp(text, "exact") == 0) return TimePrecision::Exact;
  if (strcmp(text, "anchored") == 0) return TimePrecision::Anchored;
  if (strcmp(text, "approximate") == 0) return TimePrecision::Approximate;
  return TimePrecision::Unknown;
}

bool parseRow(JsonScanner& scan, RemoteEvent& out, bool& current) {
  out = RemoteEvent{};
  current = false;
  if (!scan.enterObject()) return scan.skipValue() && false;
  char key[24];
  while (scan.nextMember(key, sizeof(key))) {
    if (strcmp(key, "recovery_id") == 0) {
      bool truncated = false;
      out.hasEventId = scan.readString(out.eventId, sizeof(out.eventId), &truncated) && !truncated && out.eventId[0];
    } else if (strcmp(key, "device_id") == 0) {
      scan.readString(out.originId, sizeof(out.originId));
    } else if (strcmp(key, "device_name") == 0) {
      scan.readString(out.originName, sizeof(out.originName));
    } else if (strcmp(key, "percentage") == 0) {
      out.hasPct = scan.readPercentMicro(out.pctMicro);
    } else if (strcmp(key, "xpointer") == 0) {
      bool truncated = false;
      out.xpointerUsable =
          scan.readString(out.xpointer, sizeof(out.xpointer), &truncated) && !truncated && out.xpointer[0];
      if (!out.xpointerUsable) out.xpointer[0] = '\0';
    } else if (strcmp(key, "occurred_at") == 0) {
      scan.readString(out.occurredAt, sizeof(out.occurredAt));
    } else if (strcmp(key, "time_precision") == 0) {
      char text[16] = {0};
      if (scan.readString(text, sizeof(text))) out.precision = precisionOf(text);
    } else if (strcmp(key, "is_current_device") == 0) {
      scan.readBool(current);
    } else {
      scan.skipValue();
    }
  }
  return !scan.failed();
}

}  // namespace

bool parseOtherPosition(JsonScanner& scan, RemoteEvent& out) {
  if (!scan.enterObject()) return false;
  char key[24];
  while (scan.nextMember(key, sizeof(key))) {
    if (strcmp(key, "positions") != 0) {
      scan.skipValue();
      continue;
    }
    if (!scan.enterArray()) return false;
    while (scan.nextElement()) {
      bool current = false;
      if (parseRow(scan, out, current) && !current && out.hasEventId && (out.hasPct || out.xpointerUsable)) return true;
    }
    return false;
  }
  return false;
}

}  // namespace borges
