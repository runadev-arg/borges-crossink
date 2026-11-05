#include "net/DnsCodec.h"

#include <cstring>

namespace borges {
namespace net {
namespace {

constexpr size_t kHeader = 12;
constexpr uint16_t kTypeA = 1;
constexpr uint16_t kClassIn = 1;

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

char lower(const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

// Moves past a name. A pointer ends it: names in the answer are never followed, because
// the question was already checked, and not expanding them leaves no loop to guard.
bool skipName(const uint8_t* m, const size_t len, size_t& pos) {
  while (pos < len) {
    const uint8_t label = m[pos];
    if (label == 0) {
      pos += 1;
      return true;
    }
    if ((label & 0xC0) == 0xC0) {
      if (pos + 2 > len) return false;
      pos += 2;
      return true;
    }
    if ((label & 0xC0) != 0) return false;
    pos += 1 + static_cast<size_t>(label);
  }
  return false;
}

// The question's name, uncompressed by definition, against the host.
bool questionMatches(const uint8_t* m, const size_t len, size_t pos, const char* host) {
  const char* h = host;
  while (pos < len) {
    const uint8_t label = m[pos++];
    if (label == 0) return *h == '\0' || (h[0] == '.' && h[1] == '\0');
    if ((label & 0xC0) != 0 || pos + label > len) return false;
    if (h != host) {
      if (*h != '.') return false;
      ++h;
    }
    for (uint8_t i = 0; i < label; ++i, ++h) {
      if (*h == '\0' || lower(static_cast<char>(m[pos + i])) != lower(*h)) return false;
    }
    pos += label;
  }
  return false;
}

}  // namespace

size_t buildDnsQuery(const char* host, const uint16_t id, uint8_t* out, const size_t cap) {
  if (host == nullptr || *host == '\0') return 0;
  const size_t hostLen = strlen(host);
  if (hostLen > 253) return 0;
  const size_t messageLen = kHeader + hostLen + 2 + 4;
  if (cap < messageLen + 2) return 0;

  uint8_t* m = out + 2;
  memset(m, 0, kHeader);
  m[0] = static_cast<uint8_t>(id >> 8);
  m[1] = static_cast<uint8_t>(id);
  m[2] = 0x01;  // RD
  m[5] = 1;     // QDCOUNT

  size_t pos = kHeader;
  const char* label = host;
  for (;;) {
    const char* dot = strchr(label, '.');
    const size_t n = dot ? static_cast<size_t>(dot - label) : strlen(label);
    if (n == 0) {
      if (dot == nullptr && label != host) break;  // A trailing dot is fine.
      return 0;
    }
    if (n > 63) return 0;
    m[pos++] = static_cast<uint8_t>(n);
    memcpy(m + pos, label, n);
    pos += n;
    if (dot == nullptr) break;
    label = dot + 1;
  }
  m[pos++] = 0;
  m[pos++] = 0;
  m[pos++] = kTypeA;
  m[pos++] = 0;
  m[pos++] = kClassIn;

  out[0] = static_cast<uint8_t>(pos >> 8);
  out[1] = static_cast<uint8_t>(pos);
  return pos + 2;
}

DnsParse parseDnsAnswer(const uint8_t* m, const size_t len, const uint16_t id, const char* host, uint8_t addr[4]) {
  if (len < kHeader) return DnsParse::TooShort;
  if (be16(m) != id) return DnsParse::BadId;
  if ((m[2] & 0x80) == 0) return DnsParse::NotAResponse;
  if ((m[3] & 0x0F) != 0) return DnsParse::ServerError;
  if (be16(m + 4) != 1) return DnsParse::Malformed;

  size_t pos = kHeader;
  if (!questionMatches(m, len, pos, host)) return DnsParse::WrongQuestion;
  if (!skipName(m, len, pos) || pos + 4 > len) return DnsParse::Malformed;
  pos += 4;

  const uint16_t answers = be16(m + 6);
  for (uint16_t i = 0; i < answers; ++i) {
    if (!skipName(m, len, pos) || pos + 10 > len) return DnsParse::Malformed;
    const uint16_t type = be16(m + pos);
    const uint16_t klass = be16(m + pos + 2);
    const uint16_t rdlen = be16(m + pos + 8);
    pos += 10;
    if (pos + rdlen > len) return DnsParse::Malformed;
    if (type == kTypeA && klass == kClassIn && rdlen == 4) {
      memcpy(addr, m + pos, 4);
      return DnsParse::Ok;
    }
    pos += rdlen;
  }
  return DnsParse::NoAddress;
}

bool plausibleServerAddress(const uint8_t a[4]) {
  if (a[0] == 0 || a[0] == 127 || a[0] >= 224) return false;
  if (a[0] == 169 && a[1] == 254) return false;
  return true;
}

}  // namespace net
}  // namespace borges
