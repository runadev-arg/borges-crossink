#pragma once

// Rule 8, as decisions. The backend does the lookups; this decides the order, the time
// each step gets, what is remembered and for how long.

#include <cstddef>
#include <cstdint>

#include "Ports.h"

namespace borges {
namespace net {

enum class DnsSource : uint8_t { Getaddrinfo, DnsOverTcp, LastGood, Baked };

struct Candidate {
  uint8_t ip[4] = {0, 0, 0, 0};
  DnsSource source = DnsSource::Baked;
};

struct Candidates {
  static constexpr uint8_t kMax = 4;
  Candidate items[kMax];
  uint8_t count = 0;
  bool add(const uint8_t ip[4], DnsSource source);  // Drops duplicates and impossible addresses.
};

class IDnsBackend {
 public:
  virtual ~IDnsBackend() = default;
  // Each is bounded by budgetMs and returns true with the address.
  virtual bool getaddrinfo(const char* host, uint32_t budgetMs, uint8_t out[4]) = 0;
  virtual bool dnsOverTcp(const uint8_t resolver[4], const char* host, uint32_t budgetMs, uint8_t out[4]) = 0;
  virtual bool lastGood(const char* host, uint8_t out[4]) = 0;
};

constexpr const char* kProductionHost = "borges.runadev.com";
constexpr uint32_t kDnsMemoMs = 5 * 60 * 1000;
// An answer that only came from a fallback is kept just long enough for the same
// session not to pay for a dead resolver twice. It never lasts for the boot.
constexpr uint32_t kDnsFallbackMemoMs = 30 * 1000;
constexpr uint32_t kGetaddrinfoMaxMs = 3000;
constexpr uint32_t kDnsTcpMaxMs = 2000;

class DnsChain {
 public:
  // nowMs: a monotonic clock. A failed chain leaves no trace.
  const Candidates& resolve(const char* host, uint32_t budgetMs, uint64_t nowMs, IDnsBackend& backend, ICancel& cancel,
                            IClock& clock);
  void forget();

 private:
  char memoHost_[64] = {0};
  uint64_t memoUntilMs_ = 0;
  Candidates memo_;
  Candidates result_;
};

}  // namespace net
}  // namespace borges
