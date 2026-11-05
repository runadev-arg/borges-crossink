#pragma once

// What the network layer keeps on the SD card, each in its own two-slot record:
//   net-link  The last network that connected, with its channel and BSSID. No password.
//   net-dns   The last address that completed a handshake.
//   net-diag  The last failure, for the Borges screen. Never a token or a URL.

#include <cstdint>

#include "Ports.h"

namespace borges {
namespace net {

struct LinkHint {
  char ssid[33] = {0};
  uint8_t bssid[6] = {0, 0, 0, 0, 0, 0};
  uint8_t channel = 0;
  bool usable() const { return ssid[0] != '\0' && channel != 0; }
};
bool loadLinkHint(IFs& fs, LinkHint& out);
bool saveLinkHint(IFs& fs, const LinkHint& hint);

struct DnsMemory {
  char host[64] = {0};
  uint8_t ip[4] = {0, 0, 0, 0};
};
bool loadDnsMemory(IFs& fs, DnsMemory& out);
bool saveDnsMemory(IFs& fs, const DnsMemory& memory);

struct NetDiag {
  TransportStage stage = TransportStage::None;
  int32_t code = 0;
  int64_t atEpoch = 0;
  char failure[20] = {0};  // failureCode() or linkResultCode().
};
bool loadNetDiag(IFs& fs, NetDiag& out);
bool saveNetDiag(IFs& fs, const NetDiag& diag);

}  // namespace net
}  // namespace borges
