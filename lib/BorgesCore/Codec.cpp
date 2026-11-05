#include "Codec.h"

#include <cstring>

namespace borges {

// Nibble table: 64 bytes of flash instead of 1 KiB, and CRCs here run over short
// records at SD speed.
uint32_t crc32Begin() { return 0xFFFFFFFFu; }

uint32_t crc32Update(uint32_t state, const void* data, size_t len) {
  static constexpr uint32_t kTable[16] = {0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
                                          0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
                                          0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    state = kTable[(state ^ p[i]) & 0x0F] ^ (state >> 4);
    state = kTable[(state ^ (p[i] >> 4)) & 0x0F] ^ (state >> 4);
  }
  return state;
}

uint32_t crc32End(uint32_t state) { return state ^ 0xFFFFFFFFu; }

uint32_t crc32(const void* data, size_t len) { return crc32End(crc32Update(crc32Begin(), data, len)); }

uint64_t fnv1a64(const void* data, size_t len, uint64_t seed) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint64_t h = seed;
  for (size_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

bool parseSequence(const char* text, uint64_t& out) {
  if (!text || !*text) return false;
  if (text[0] == '0' && text[1] != '\0') return false;
  uint64_t value = 0;
  for (const char* p = text; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
    const uint64_t digit = static_cast<uint64_t>(*p - '0');
    if (value > (0x7FFFFFFFFFFFFFFFULL - digit) / 10) return false;
    value = value * 10 + digit;
  }
  out = value;
  return true;
}

size_t formatU64(uint64_t value, char* out) {
  char tmp[21];
  size_t n = 0;
  do {
    tmp[n++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value);
  for (size_t i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
  out[n] = '\0';
  return n;
}

namespace {
constexpr char kHex[] = "0123456789abcdef";

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
}  // namespace

void hexEncode(const uint8_t* data, size_t len, char* out) {
  for (size_t i = 0; i < len; ++i) {
    out[i * 2] = kHex[data[i] >> 4];
    out[i * 2 + 1] = kHex[data[i] & 0x0F];
  }
  out[len * 2] = '\0';
}

bool hexDecode(const char* text, uint8_t* out, size_t outLen) {
  for (size_t i = 0; i < outLen; ++i) {
    const int hi = hexValue(text[i * 2]);
    if (hi < 0) return false;
    const int lo = hexValue(text[i * 2 + 1]);
    if (lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return text[outLen * 2] == '\0';
}

bool isHex(const char* text, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    if (hexValue(text[i]) < 0) return false;
  }
  return text[len] == '\0';
}

void uuidFormat(const uint8_t b[16], char out[37]) {
  size_t o = 0;
  for (size_t i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
    out[o++] = kHex[b[i] >> 4];
    out[o++] = kHex[b[i] & 0x0F];
  }
  out[o] = '\0';
}

bool uuidParse(const char* text, uint8_t out[16]) {
  size_t o = 0;
  for (size_t i = 0; i < kUuidTextLen;) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (text[i] != '-') return false;
      ++i;
      continue;
    }
    const int hi = hexValue(text[i]);
    if (hi < 0) return false;
    const int lo = hexValue(text[i + 1]);
    if (lo < 0) return false;
    out[o++] = static_cast<uint8_t>((hi << 4) | lo);
    i += 2;
  }
  return text[kUuidTextLen] == '\0';
}

bool isCanonicalUuid(const char* text) {
  uint8_t b[16];
  if (!uuidParse(text, b)) return false;
  const uint8_t version = b[6] >> 4;
  const uint8_t variant = b[8] >> 6;
  return version >= 1 && version <= 8 && variant == 2;
}

void uuidV4(IRandom& random, uint8_t out[16]) {
  random.fill(out, 16);
  out[6] = static_cast<uint8_t>((out[6] & 0x0F) | 0x40);
  out[8] = static_cast<uint8_t>((out[8] & 0x3F) | 0x80);
}

namespace {
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool digits(const char* p, int count, int& out) {
  int value = 0;
  for (int i = 0; i < count; ++i) {
    if (p[i] < '0' || p[i] > '9') return false;
    value = value * 10 + (p[i] - '0');
  }
  out = value;
  return true;
}
}  // namespace

bool parseRfc3339(const char* t, int64_t& epochSec) {
  int year, month, day, hour, minute, second;
  if (!t || !digits(t, 4, year) || t[4] != '-' || !digits(t + 5, 2, month) || t[7] != '-' || !digits(t + 8, 2, day) ||
      t[10] != 'T' || !digits(t + 11, 2, hour) || t[13] != ':' || !digits(t + 14, 2, minute) || t[16] != ':' ||
      !digits(t + 17, 2, second)) {
    return false;
  }
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return false;
  const char* p = t + 19;
  if (*p == '.') {
    ++p;
    const char* start = p;
    while (*p >= '0' && *p <= '9') ++p;
    if (p == start) return false;
  }
  int offsetSec = 0;
  if (*p == 'Z') {
    ++p;
  } else if (*p == '+' || *p == '-') {
    int oh, om;
    if (!digits(p + 1, 2, oh) || p[3] != ':' || !digits(p + 4, 2, om)) return false;
    offsetSec = (oh * 3600 + om * 60) * (*p == '-' ? -1 : 1);
    p += 6;
  } else {
    return false;
  }
  if (*p != '\0') return false;
  epochSec = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 + hour * 3600 +
             minute * 60 + second - offsetSec;
  return true;
}

void formatRfc3339(int64_t epochSec, char out[21]) {
  int64_t days = epochSec / 86400;
  int64_t rem = epochSec % 86400;
  if (rem < 0) {
    rem += 86400;
    --days;
  }
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  const int64_t y = static_cast<int64_t>(yoe) + era * 400 + (m <= 2 ? 1 : 0);
  const unsigned h = static_cast<unsigned>(rem / 3600);
  const unsigned mi = static_cast<unsigned>(rem % 3600 / 60);
  const unsigned s = static_cast<unsigned>(rem % 60);
  const auto two = [&](size_t at, unsigned v) {
    out[at] = static_cast<char>('0' + (v / 10) % 10);
    out[at + 1] = static_cast<char>('0' + v % 10);
  };
  two(0, static_cast<unsigned>(y / 100));
  two(2, static_cast<unsigned>(y % 100));
  out[4] = '-';
  two(5, m);
  out[7] = '-';
  two(8, d);
  out[10] = 'T';
  two(11, h);
  out[13] = ':';
  two(14, mi);
  out[16] = ':';
  two(17, s);
  out[19] = 'Z';
  out[20] = '\0';
}

namespace {
int base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}
}  // namespace

int32_t base64Decode(const char* text, uint8_t* out, size_t cap) {
  uint32_t acc = 0;
  int bits = 0;
  size_t n = 0;
  bool padded = false;
  for (const char* p = text; *p; ++p) {
    if (*p == '=') {
      padded = true;
      continue;
    }
    if (padded) return -1;
    const int v = base64Value(*p);
    if (v < 0) return -1;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= cap) return -1;
      out[n++] = static_cast<uint8_t>(acc >> bits);
      acc &= (1u << bits) - 1u;
    }
  }
  return static_cast<int32_t>(n);
}

void base64UrlEncode(const uint8_t* data, size_t len, char* out) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  size_t o = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < len; ++i) {
    acc = (acc << 8) | data[i];
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out[o++] = kAlphabet[(acc >> bits) & 0x3F];
    }
    acc &= (1u << bits) - 1u;
  }
  if (bits > 0) out[o++] = kAlphabet[(acc << (6 - bits)) & 0x3F];
  out[o] = '\0';
}

size_t utf8Copy(char* dst, size_t cap, const char* src, size_t srcLen) {
  if (cap == 0) return 0;
  size_t n = srcLen < cap - 1 ? srcLen : cap - 1;
  if (n < srcLen) {
    // Back up over continuation bytes, then the cut sequence is simply left out.
    while (n > 0 && (static_cast<uint8_t>(src[n]) & 0xC0) == 0x80) --n;
  }
  if (n > 0) memcpy(dst, src, n);
  dst[n] = '\0';
  return n;
}

void copyText(char* dst, size_t cap, const char* src) { utf8Copy(dst, cap, src ? src : "", src ? strlen(src) : 0); }

Md5::Md5() {
  state_[0] = 0x67452301;
  state_[1] = 0xefcdab89;
  state_[2] = 0x98badcfe;
  state_[3] = 0x10325476;
}

void Md5::block(const uint8_t* chunk) {
  static constexpr uint8_t kShift[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
  static constexpr uint32_t kSine[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
      0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
      0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
      0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
      0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
      0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
      0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
      0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
  uint32_t w[16];
  for (size_t i = 0; i < 16; ++i) {
    w[i] = static_cast<uint32_t>(chunk[i * 4]) | static_cast<uint32_t>(chunk[i * 4 + 1]) << 8 |
           static_cast<uint32_t>(chunk[i * 4 + 2]) << 16 | static_cast<uint32_t>(chunk[i * 4 + 3]) << 24;
  }
  uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  for (uint32_t i = 0; i < 64; ++i) {
    uint32_t f, g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) % 16;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) % 16;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) % 16;
    }
    const uint32_t tmp = d;
    d = c;
    c = b;
    const uint32_t sum = a + f + kSine[i] + w[g];
    const uint8_t s = kShift[(i / 16) * 4 + i % 4];
    b = b + ((sum << s) | (sum >> (32 - s)));
    a = tmp;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
}

void Md5::update(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  size_t fill = static_cast<size_t>(total_ % 64);
  total_ += len;
  if (fill) {
    const size_t take = len < 64 - fill ? len : 64 - fill;
    memcpy(buffer_ + fill, p, take);
    p += take;
    len -= take;
    fill += take;
    if (fill < 64) return;
    block(buffer_);
  }
  while (len >= 64) {
    block(p);
    p += 64;
    len -= 64;
  }
  if (len) memcpy(buffer_, p, len);
}

void Md5::finish(uint8_t digest[16]) {
  const uint64_t bits = total_ * 8;
  const uint8_t pad = 0x80;
  update(&pad, 1);
  const uint8_t zero = 0;
  while (total_ % 64 != 56) update(&zero, 1);
  uint8_t lenBytes[8];
  for (size_t i = 0; i < 8; ++i) lenBytes[i] = static_cast<uint8_t>(bits >> (8 * i));
  update(lenBytes, 8);
  for (size_t i = 0; i < 4; ++i) {
    for (size_t j = 0; j < 4; ++j) digest[i * 4 + j] = static_cast<uint8_t>(state_[i] >> (8 * j));
  }
}

void uuidFromSeed(const void* seed, size_t len, uint8_t out[16]) {
  Md5 md5;
  md5.update(seed, len);
  md5.finish(out);
  out[6] = static_cast<uint8_t>((out[6] & 0x0F) | 0x30);
  out[8] = static_cast<uint8_t>((out[8] & 0x3F) | 0x80);
}

void ByteWriter::put(const void* data, size_t len) {
  if (!ok_ || len > cap_ - len_) {
    ok_ = false;
    return;
  }
  if (len) memcpy(buf_ + len_, data, len);
  len_ += len;
}

void ByteWriter::u16(uint16_t v) {
  const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
  put(b, 2);
}

void ByteWriter::u32(uint32_t v) {
  uint8_t b[4];
  for (size_t i = 0; i < 4; ++i) b[i] = static_cast<uint8_t>(v >> (8 * i));
  put(b, 4);
}

void ByteWriter::u64(uint64_t v) {
  uint8_t b[8];
  for (size_t i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(v >> (8 * i));
  put(b, 8);
}

void ByteWriter::str(const char* text) {
  const size_t n = text ? strlen(text) : 0;
  if (n > 0xFFFF) {
    ok_ = false;
    return;
  }
  u16(static_cast<uint16_t>(n));
  put(text, n);
}

bool ByteReader::take(void* out, size_t len) {
  if (!ok_ || len > len_ - pos_) {
    ok_ = false;
    return false;
  }
  if (out && len) memcpy(out, buf_ + pos_, len);
  pos_ += len;
  return true;
}

uint8_t ByteReader::u8() {
  uint8_t v = 0;
  take(&v, 1);
  return v;
}

uint16_t ByteReader::u16() {
  uint8_t b[2] = {0, 0};
  take(b, 2);
  return static_cast<uint16_t>(b[0] | b[1] << 8);
}

uint32_t ByteReader::u32() {
  uint8_t b[4] = {0, 0, 0, 0};
  take(b, 4);
  uint32_t v = 0;
  for (size_t i = 0; i < 4; ++i) v |= static_cast<uint32_t>(b[i]) << (8 * i);
  return v;
}

uint64_t ByteReader::u64() {
  uint8_t b[8] = {0};
  take(b, 8);
  uint64_t v = 0;
  for (size_t i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b[i]) << (8 * i);
  return v;
}

void ByteReader::bytes(void* out, size_t len) { take(out, len); }

size_t ByteReader::str(char* out, size_t cap) {
  const uint16_t n = u16();
  if (!ok_ || cap == 0 || n >= cap) {
    ok_ = false;
    if (cap) out[0] = '\0';
    return 0;
  }
  if (!take(out, n)) {
    out[0] = '\0';
    return 0;
  }
  out[n] = '\0';
  return n;
}

void JsonWriter::put(const char* data, size_t len) {
  if (buf_ && len_ + len <= cap_ && len) memcpy(buf_ + len_, data, len);
  len_ += len;
}

void JsonWriter::raw(const char* text) { put(text, strlen(text)); }
void JsonWriter::rawBytes(const char* data, size_t len) { put(data, len); }

void JsonWriter::sep() {
  if (needComma_) put(",", 1);
}

void JsonWriter::objectBegin() {
  sep();
  put("{", 1);
  needComma_ = false;
}

void JsonWriter::objectEnd() {
  put("}", 1);
  needComma_ = true;
}

void JsonWriter::arrayBegin() {
  sep();
  put("[", 1);
  needComma_ = false;
}

void JsonWriter::arrayEnd() {
  put("]", 1);
  needComma_ = true;
}

void JsonWriter::escaped(const char* text, size_t len) {
  put("\"", 1);
  escapedRun(text, len);
  put("\"", 1);
}

void JsonWriter::escapedRun(const char* text, size_t len) {
  size_t runStart = 0;
  for (size_t i = 0; i < len; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c >= 0x20 && c != '"' && c != '\\') continue;
    put(text + runStart, i - runStart);
    runStart = i + 1;
    char esc[6] = {'\\', 'u', '0', '0', kHex[c >> 4], kHex[c & 0x0F]};
    if (c == '"' || c == '\\') {
      esc[1] = static_cast<char>(c);
      put(esc, 2);
    } else if (c == '\n') {
      esc[1] = 'n';
      put(esc, 2);
    } else {
      put(esc, 6);
    }
  }
  put(text + runStart, len - runStart);
}

void JsonWriter::key(const char* name) {
  sep();
  escaped(name, strlen(name));
  put(":", 1);
  needComma_ = false;
}

void JsonWriter::value(const char* text) { value(text, strlen(text)); }

void JsonWriter::value(const char* text, size_t len) {
  sep();
  escaped(text, len);
  needComma_ = true;
}

void JsonWriter::valueNormalizedText(const char* text) {
  sep();
  put("\"", 1);
  const char* run = text;
  for (const char* p = text;; ++p) {
    if (*p != '\0' && *p != '\r') continue;
    escapedRun(run, static_cast<size_t>(p - run));
    if (*p == '\0') break;
    if (p[1] != '\n') put("\\n", 2);
    run = p + 1;
  }
  put("\"", 1);
  needComma_ = true;
}

void JsonWriter::valueSequence(uint64_t v) {
  char tmp[21];
  const size_t n = formatU64(v, tmp);
  value(tmp, n);
}

void JsonWriter::valueInt(int64_t v) {
  sep();
  char tmp[21];
  if (v < 0) {
    put("-", 1);
    const size_t n = formatU64(static_cast<uint64_t>(-v), tmp);
    put(tmp, n);
  } else {
    const size_t n = formatU64(static_cast<uint64_t>(v), tmp);
    put(tmp, n);
  }
  needComma_ = true;
}

void JsonWriter::valueBool(bool v) {
  sep();
  raw(v ? "true" : "false");
  needComma_ = true;
}

void JsonWriter::valueNull() {
  sep();
  raw("null");
  needComma_ = true;
}

void JsonWriter::valuePercent(uint32_t micro) {
  sep();
  char tmp[21];
  const uint32_t whole = micro / 1000000u;
  uint32_t frac = micro % 1000000u;
  const size_t n = formatU64(whole, tmp);
  put(tmp, n);
  if (frac) {
    char decimals[6];
    for (int i = 5; i >= 0; --i) {
      decimals[i] = static_cast<char>('0' + frac % 10);
      frac /= 10;
    }
    size_t used = 6;
    while (used > 0 && decimals[used - 1] == '0') --used;
    put(".", 1);
    put(decimals, used);
  }
  needComma_ = true;
}

void JsonWriter::valueUuid(const uint8_t bytes[16]) {
  char text[37];
  uuidFormat(bytes, text);
  value(text, kUuidTextLen);
}

const char* precisionName(TimePrecision p) {
  switch (p) {
    case TimePrecision::Exact:
      return "exact";
    case TimePrecision::Anchored:
      return "anchored";
    case TimePrecision::Approximate:
      return "approximate";
    default:
      return "unknown";
  }
}

bool parsePrecision(const char* text, TimePrecision& out) {
  static constexpr TimePrecision kAll[] = {TimePrecision::Unknown, TimePrecision::Approximate, TimePrecision::Anchored,
                                           TimePrecision::Exact};
  for (TimePrecision p : kAll) {
    if (strcmp(text, precisionName(p)) == 0) {
      out = p;
      return true;
    }
  }
  return false;
}

}  // namespace borges
