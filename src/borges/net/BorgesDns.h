#pragma once

// Rule 8. getaddrinfo -> DNS over TCP (1.1.1.1, 8.8.8.8) -> last good IP -> baked
// Cloudflare IPs (production host only). The decisions live in net/DnsChain; this is
// the lwIP and socket backend. Never calls Network.hostByName().

#include "Ports.h"
#include "net/DnsChain.h"

namespace borges {
namespace net {

class BorgesDns {
 public:
  explicit BorgesDns(IFs& fs);
  // Candidates, best first. count == 0 means dns_failed; nothing is remembered about
  // a failure, so the next call tries the whole chain again.
  const Candidates& resolve(const char* host, uint32_t budgetMs, ICancel& cancel);
  // The address that actually completed a handshake: becomes the "last good IP".
  void remember(const char* host, const uint8_t ip[4]);

 private:
  IFs& fs_;
  DnsChain chain_;
};

}  // namespace net
}  // namespace borges
