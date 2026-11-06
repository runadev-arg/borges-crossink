#pragma once

// Plain values shared by the modules. No behaviour beyond trivial helpers.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Codec.h"
#include "Ports.h"

namespace borges {

// Longest xpointer kept on the device. The server allows 4096, but one that does
// not fit is reported as "no usable xpointer" (approximate), never truncated: half
// an xpointer points somewhere wrong.
constexpr size_t kMaxXpointer = 512;

// A book's exact edition: KOReader's partial MD5.
struct BookId {
  uint8_t md5[16] = {0};
  char hex[33] = {0};

  bool setHex(const char* text) {
    if (!text || strlen(text) != 32 || !hexDecode(text, md5, 16)) return false;
    hexEncode(md5, 16, hex);  // Normalized to lowercase, as the server does.
    return true;
  }
  void setRaw(const uint8_t raw[16]) {
    memcpy(md5, raw, 16);
    hexEncode(md5, 16, hex);
  }
  bool valid() const { return hex[0] != '\0'; }
};

// What the core takes from and returns to the reader. Converting to and from pages
// and spine offsets is the glue's job. Percent is in millionths: 12.5% is 12500000.
struct Position {
  uint32_t pctMicro = 0;
  bool hasPct = false;
  int32_t page = -1;
  int32_t totalPages = -1;
  char xpointer[kMaxXpointer] = {0};

  void setPercent(double percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    pctMicro = static_cast<uint32_t>(percent * 1000000.0 + 0.5);
    hasPct = true;
  }
};

enum class SyncStage : uint8_t {
  None,
  Capabilities,
  Focus,
  Pull,
  Drain,
  Commit,
  Suggestion,
  Rotate,
  Health,
  Pairing,
  Catalog
};

// Why the last session stopped, in a form the Borges screen can show. Never holds a
// token: code and detail only come from error bodies and local constants.
struct Diagnostics {
  SyncStage stage = SyncStage::None;
  int16_t httpStatus = 0;
  TransportStage transportStage = TransportStage::None;
  int32_t transportCode = 0;
  int64_t atEpoch = 0;
  char code[40] = {0};
  char detail[64] = {0};
  char requestId[40] = {0};

  bool hasError() const { return code[0] != '\0'; }
  void clear() { *this = Diagnostics{}; }
};

}  // namespace borges
