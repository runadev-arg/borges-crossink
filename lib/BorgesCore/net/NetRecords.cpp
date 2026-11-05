#include "net/NetRecords.h"

#include <cstring>

#include "AtomicFile.h"
#include "Store.h"

namespace borges {
namespace net {
namespace {

constexpr const char* kLinkPath = "/.crosspoint/borges/net-link";
constexpr const char* kDnsPath = "/.crosspoint/borges/net-dns";
constexpr const char* kDiagPath = "/.crosspoint/borges/net-diag";
constexpr uint8_t kVersion = 1;

bool loadRecord(IFs& fs, const char* path, uint8_t* buf, const size_t want) {
  AtomicFile file(fs, path);
  size_t len = 0;
  return file.load(buf, want, len) && len == want && buf[0] == kVersion;
}

bool saveRecord(IFs& fs, const char* path, const uint8_t* buf, const size_t len) {
  fs.mkdir(kBorgesDir);
  AtomicFile file(fs, path);
  return file.save(buf, len);
}

void putI32(uint8_t* p, const int32_t v) {
  const auto u = static_cast<uint32_t>(v);
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(u >> (8 * i));
}
int32_t getI32(const uint8_t* p) {
  uint32_t u = 0;
  for (int i = 0; i < 4; ++i) u |= static_cast<uint32_t>(p[i]) << (8 * i);
  return static_cast<int32_t>(u);
}
void putI64(uint8_t* p, const int64_t v) {
  const auto u = static_cast<uint64_t>(v);
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(u >> (8 * i));
}
int64_t getI64(const uint8_t* p) {
  uint64_t u = 0;
  for (int i = 0; i < 8; ++i) u |= static_cast<uint64_t>(p[i]) << (8 * i);
  return static_cast<int64_t>(u);
}

}  // namespace

bool loadLinkHint(IFs& fs, LinkHint& out) {
  uint8_t b[1 + sizeof(out.ssid) + 6 + 1];
  if (!loadRecord(fs, kLinkPath, b, sizeof(b))) return false;
  LinkHint hint;
  memcpy(hint.ssid, b + 1, sizeof(hint.ssid));
  hint.ssid[sizeof(hint.ssid) - 1] = '\0';
  memcpy(hint.bssid, b + 1 + sizeof(hint.ssid), 6);
  hint.channel = b[1 + sizeof(hint.ssid) + 6];
  out = hint;
  return true;
}

bool saveLinkHint(IFs& fs, const LinkHint& hint) {
  uint8_t b[1 + sizeof(hint.ssid) + 6 + 1];
  b[0] = kVersion;
  memcpy(b + 1, hint.ssid, sizeof(hint.ssid));
  memcpy(b + 1 + sizeof(hint.ssid), hint.bssid, 6);
  b[1 + sizeof(hint.ssid) + 6] = hint.channel;
  return saveRecord(fs, kLinkPath, b, sizeof(b));
}

bool loadDnsMemory(IFs& fs, DnsMemory& out) {
  uint8_t b[1 + sizeof(out.host) + 4];
  if (!loadRecord(fs, kDnsPath, b, sizeof(b))) return false;
  DnsMemory memory;
  memcpy(memory.host, b + 1, sizeof(memory.host));
  memory.host[sizeof(memory.host) - 1] = '\0';
  memcpy(memory.ip, b + 1 + sizeof(memory.host), 4);
  out = memory;
  return true;
}

bool saveDnsMemory(IFs& fs, const DnsMemory& memory) {
  uint8_t b[1 + sizeof(memory.host) + 4];
  b[0] = kVersion;
  memcpy(b + 1, memory.host, sizeof(memory.host));
  memcpy(b + 1 + sizeof(memory.host), memory.ip, 4);
  return saveRecord(fs, kDnsPath, b, sizeof(b));
}

bool loadNetDiag(IFs& fs, NetDiag& out) {
  uint8_t b[1 + 1 + 4 + 8 + sizeof(out.failure)];
  if (!loadRecord(fs, kDiagPath, b, sizeof(b))) return false;
  NetDiag diag;
  diag.stage = static_cast<TransportStage>(b[1]);
  diag.code = getI32(b + 2);
  diag.atEpoch = getI64(b + 6);
  memcpy(diag.failure, b + 14, sizeof(diag.failure));
  diag.failure[sizeof(diag.failure) - 1] = '\0';
  out = diag;
  return true;
}

bool saveNetDiag(IFs& fs, const NetDiag& diag) {
  uint8_t b[1 + 1 + 4 + 8 + sizeof(diag.failure)];
  b[0] = kVersion;
  b[1] = static_cast<uint8_t>(diag.stage);
  putI32(b + 2, diag.code);
  putI64(b + 6, diag.atEpoch);
  memcpy(b + 14, diag.failure, sizeof(diag.failure));
  return saveRecord(fs, kDiagPath, b, sizeof(b));
}

}  // namespace net
}  // namespace borges
