#pragma once

// Small value codecs shared by every module: checksums, decimal sequences, UUIDs,
// RFC 3339, base64, MD5, a binary reader/writer and a bounded JSON writer. Nothing
// here allocates or touches IO.

#include <cstddef>
#include <cstdint>

#include "Ports.h"

namespace borges {

uint32_t crc32Begin();
uint32_t crc32Update(uint32_t state, const void* data, size_t len);
uint32_t crc32End(uint32_t state);
uint32_t crc32(const void* data, size_t len);

constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ULL;
uint64_t fnv1a64(const void* data, size_t len, uint64_t seed = kFnvBasis);

// Decimal sequence as the server serializes it: ^(0|[1-9]\d*)$, at most INT64_MAX.
bool parseSequence(const char* text, uint64_t& out);
size_t formatU64(uint64_t value, char* out);  // NUL-terminated, out holds >= 21 bytes.

void hexEncode(const uint8_t* data, size_t len, char* out);  // Lowercase, NUL-terminated.
bool hexDecode(const char* text, uint8_t* out, size_t outLen);
bool isHex(const char* text, size_t len);

constexpr size_t kUuidTextLen = 36;
void uuidFormat(const uint8_t bytes[16], char out[37]);
bool uuidParse(const char* text, uint8_t out[16]);
bool isCanonicalUuid(const char* text);  // Same shape the server's UUID_RE accepts.
void uuidV4(IRandom& random, uint8_t out[16]);

// Epoch seconds <-> "YYYY-MM-DDTHH:MM:SS[.fff](Z|+hh:mm)". Fractions are dropped.
bool parseRfc3339(const char* text, int64_t& epochSec);
void formatRfc3339(int64_t epochSec, char out[21]);

// Returns decoded length, or -1 on invalid input or lack of room.
int32_t base64Decode(const char* text, uint8_t* out, size_t cap);
// Base64url without padding. out holds >= 4*(len/3)+4 bytes.
void base64UrlEncode(const uint8_t* data, size_t len, char* out);

// Copies at most cap-1 bytes, never splitting a UTF-8 sequence. Returns bytes copied.
size_t utf8Copy(char* dst, size_t cap, const char* src, size_t srcLen);
void copyText(char* dst, size_t cap, const char* src);  // Same, for NUL-terminated input.

class Md5 {
 public:
  Md5();
  void update(const void* data, size_t len);
  void finish(uint8_t digest[16]);

 private:
  void block(const uint8_t* chunk);
  uint32_t state_[4];
  uint64_t total_ = 0;
  uint8_t buffer_[64];
};

// Derives a stable UUID (version 3 shape) from a seed. Same recipe as the KOReader plugin.
void uuidFromSeed(const void* seed, size_t len, uint8_t out[16]);

// Sequential little-endian writer over a caller buffer. Overflow sticks.
class ByteWriter {
 public:
  ByteWriter(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}
  void u8(uint8_t v) { put(&v, 1); }
  void u16(uint16_t v);
  void u32(uint32_t v);
  void u64(uint64_t v);
  void bytes(const void* data, size_t len) { put(data, len); }
  void str(const char* text);  // u16 length + bytes.
  bool ok() const { return ok_; }
  size_t size() const { return len_; }

 private:
  void put(const void* data, size_t len);
  uint8_t* buf_;
  size_t cap_;
  size_t len_ = 0;
  bool ok_ = true;
};

class ByteReader {
 public:
  ByteReader(const uint8_t* buf, size_t len) : buf_(buf), len_(len) {}
  uint8_t u8();
  uint16_t u16();
  uint32_t u32();
  uint64_t u64();
  void bytes(void* out, size_t len);
  size_t str(char* out, size_t cap);  // Returns the length. Too long for cap makes the reader fail.
  bool ok() const { return ok_; }
  size_t remaining() const { return len_ - pos_; }

 private:
  bool take(void* out, size_t len);
  const uint8_t* buf_;
  size_t len_;
  size_t pos_ = 0;
  bool ok_ = true;
};

// Bounded JSON writer. With a null buffer it only counts, so a caller can size an
// exact allocation in a first pass. Commas are automatic. It never allocates.
class JsonWriter {
 public:
  JsonWriter(char* buf, size_t cap) : buf_(buf), cap_(cap) {}
  void raw(const char* text);
  void rawBytes(const char* data, size_t len);
  void objectBegin();
  void objectEnd();
  void arrayBegin();
  void arrayEnd();
  void key(const char* name);
  void value(const char* text);
  void value(const char* text, size_t len);
  void valueSequence(uint64_t v);  // Decimal string, as every 64-bit integer on this wire.
  void valueInt(int64_t v);
  void valueBool(bool v);
  void valueNull();
  void valuePercent(uint32_t micro);  // 0..100000000 as a JSON number with up to 6 decimals.
  void valueUuid(const uint8_t bytes[16]);
  void markValue() { needComma_ = true; }  // After rawBytes() wrote a complete value.
  void kv(const char* k, const char* v) {
    key(k);
    value(v);
  }
  void kvNull(const char* k) {
    key(k);
    valueNull();
  }
  void kvInt(const char* k, int64_t v) {
    key(k);
    valueInt(v);
  }
  void kvSeq(const char* k, uint64_t v) {
    key(k);
    valueSequence(v);
  }
  void kvBool(const char* k, bool v) {
    key(k);
    valueBool(v);
  }
  size_t size() const { return len_; }
  bool ok() const { return buf_ == nullptr || len_ <= cap_; }

 private:
  void put(const char* data, size_t len);
  void sep();
  void escaped(const char* text, size_t len);
  char* buf_;
  size_t cap_;
  size_t len_ = 0;
  bool needComma_ = false;
};

const char* precisionName(TimePrecision p);
bool parsePrecision(const char* text, TimePrecision& out);

}  // namespace borges
