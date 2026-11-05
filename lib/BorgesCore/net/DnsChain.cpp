#include "net/DnsChain.h"

#include <cstring>

#include "net/DnsCodec.h"

namespace borges {
namespace net {
namespace {

// Verified with curl --resolve on 2026-11-05: both answered the capabilities route with
// a certificate that verified. They are anycast and can change, so they are the last
// resort, and only for the host they belong to.
constexpr uint8_t kBakedIps[][4] = {{104, 21, 0, 166}, {172, 67, 128, 30}};
constexpr uint8_t kResolvers[][4] = {{1, 1, 1, 1}, {8, 8, 8, 8}};

bool sameHost(const char* a, const char* b) {
  for (;; ++a, ++b) {
    const char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
    const char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
    if (ca != cb) return false;
    if (ca == '\0') return true;
  }
}

uint32_t minOf(const uint32_t a, const uint32_t b) { return a < b ? a : b; }

}  // namespace

bool Candidates::add(const uint8_t ip[4], const DnsSource source) {
  if (count >= kMax || !plausibleServerAddress(ip)) return false;
  for (uint8_t i = 0; i < count; ++i) {
    if (memcmp(items[i].ip, ip, 4) == 0) return false;
  }
  memcpy(items[count].ip, ip, 4);
  items[count].source = source;
  ++count;
  return true;
}

void DnsChain::forget() {
  memoHost_[0] = '\0';
  memoUntilMs_ = 0;
}

const Candidates& DnsChain::resolve(const char* host, const uint32_t budgetMs, const uint64_t nowMs,
                                    IDnsBackend& backend, ICancel& cancel, IClock& clock) {
  result_ = Candidates{};
  if (host == nullptr || *host == '\0') return result_;
  if (memoHost_[0] != '\0' && nowMs < memoUntilMs_ && sameHost(memoHost_, host)) {
    result_ = memo_;
    return result_;
  }

  const uint64_t deadline = nowMs + budgetMs;
  const auto remaining = [&]() -> uint32_t {
    const uint64_t t = clock.monotonicMs();
    return t >= deadline ? 0 : static_cast<uint32_t>(deadline - t);
  };

  uint8_t ip[4];
  bool live = false;
  uint32_t left = remaining();
  if (left > 0 && !cancel.cancelled() && backend.getaddrinfo(host, minOf(kGetaddrinfoMaxMs, left), ip)) {
    live = result_.add(ip, DnsSource::Getaddrinfo);
  }
  if (!live) {
    for (const auto& resolver : kResolvers) {
      left = remaining();
      if (left == 0 || cancel.cancelled()) break;
      if (backend.dnsOverTcp(resolver, host, minOf(kDnsTcpMaxMs, left), ip) && result_.add(ip, DnsSource::DnsOverTcp)) {
        live = true;
        break;
      }
    }
  }

  // The fallbacks cost nothing and are always added: a live answer that turns out to
  // refuse connections still has somewhere to go.
  if (backend.lastGood(host, ip)) result_.add(ip, DnsSource::LastGood);
  if (sameHost(host, kProductionHost)) {
    for (const auto& baked : kBakedIps) result_.add(baked, DnsSource::Baked);
  }

  if (result_.count > 0 && strlen(host) < sizeof(memoHost_)) {
    strcpy(memoHost_, host);
    memo_ = result_;
    memoUntilMs_ = clock.monotonicMs() + (live ? kDnsMemoMs : kDnsFallbackMemoMs);
  } else {
    forget();
  }
  return result_;
}

}  // namespace net
}  // namespace borges
