#include "BorgesDns.h"

#include <Arduino.h>
#if !defined(SIMULATOR)
#include <WiFiClient.h>
#endif

#include <atomic>
#include <cstring>

#include "net/DnsCodec.h"
#include "net/NetRecords.h"

#if defined(BORGES_NET_PROBE)
#include <ws2tcpip.h>
extern bool gProbeFailGetaddrinfo;
#elif !defined(SIMULATOR)
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#endif

namespace borges {
namespace net {
namespace {

// Rule 8 needs a monotonic time here and nowhere else, and the clock owns this class.
class MillisClock : public IClock {
 public:
  uint64_t monotonicMs() override {
    // millis() is 32 bits and wraps at 49 days; a session is minutes long.
    return millis();
  }
  WallTime wallNow() override { return WallTime{}; }
  void setWall(int64_t, TimePrecision) override {}
};

uint16_t randomId() {
#if defined(SIMULATOR) || defined(BORGES_NET_PROBE)
  return static_cast<uint16_t>(rand());
#else
  return static_cast<uint16_t>(esp_random());
#endif
}

#if !defined(SIMULATOR) && !defined(BORGES_NET_PROBE)

// lwip_getaddrinfo blocks for as long as lwIP keeps retrying a dead resolver, which is
// longer than the 12 s open session. It runs on its own task so the caller keeps its
// deadline; a call that is given up on finishes later, into this static block, and frees
// its own stack. Never more than one is alive.
struct GaiJob {
  std::atomic<uint8_t> state{0};  // 0 idle, 1 running, 2 done
  char host[64] = {0};
  uint8_t ip[4] = {0, 0, 0, 0};
  bool ok = false;
};
GaiJob sGai;

void gaiTask(void*) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  const int err = lwip_getaddrinfo(sGai.host, nullptr, &hints, &found);
  sGai.ok = false;
  if (err == 0 && found != nullptr && found->ai_family == AF_INET) {
    const auto* v4 = reinterpret_cast<const sockaddr_in*>(found->ai_addr);
    memcpy(sGai.ip, &v4->sin_addr.s_addr, 4);
    sGai.ok = true;
  }
  if (found != nullptr) lwip_freeaddrinfo(found);
  sGai.state.store(2);
  vTaskDelete(nullptr);
}

bool systemResolve(const char* host, const uint32_t budgetMs, uint8_t out[4]) {
  if (strlen(host) >= sizeof(sGai.host)) return false;
  if (sGai.state.load() == 1) return false;  // One given up on is still blocked in lwIP.
  strcpy(sGai.host, host);
  sGai.state.store(1);
  if (xTaskCreate(gaiTask, "borges-dns", 3072, nullptr, uxTaskPriorityGet(nullptr), nullptr) != pdPASS) {
    sGai.state.store(0);
    return false;
  }
  const uint32_t start = millis();
  while (millis() - start < budgetMs) {
    if (sGai.state.load() == 2) {
      const bool ok = sGai.ok;
      if (ok) memcpy(out, sGai.ip, 4);
      sGai.state.store(0);
      return ok;
    }
    delay(20);
  }
  return false;
}

#elif defined(BORGES_NET_PROBE)

bool systemResolve(const char* host, uint32_t, uint8_t out[4]) {
  if (::gProbeFailGetaddrinfo) return false;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  if (getaddrinfo(host, nullptr, &hints, &found) != 0 || found == nullptr) return false;
  memcpy(out, &reinterpret_cast<const sockaddr_in*>(found->ai_addr)->sin_addr, 4);
  freeaddrinfo(found);
  return true;
}

#endif

#if !defined(SIMULATOR)

bool readExact(WiFiClient& client, uint8_t* out, const size_t count, const uint32_t deadline) {
  size_t got = 0;
  while (got < count) {
    if (static_cast<int32_t>(millis() - deadline) >= 0) return false;
    const int n = client.read(out + got, count - got);
    if (n > 0) {
      got += static_cast<size_t>(n);
    } else if (!client.connected() && client.available() == 0) {
      return false;
    } else {
      delay(5);
    }
  }
  return true;
}

class LiveBackend : public IDnsBackend {
 public:
  explicit LiveBackend(IFs& fs) : fs_(fs) {}

  bool getaddrinfo(const char* host, const uint32_t budgetMs, uint8_t out[4]) override {
    return systemResolve(host, budgetMs, out);
  }

  bool dnsOverTcp(const uint8_t resolver[4], const char* host, const uint32_t budgetMs, uint8_t out[4]) override {
    uint8_t query[kDnsMaxQuery];
    const uint16_t id = randomId();
    const size_t queryLen = buildDnsQuery(host, id, query, sizeof(query));
    if (queryLen == 0) return false;

    const uint32_t deadline = millis() + budgetMs;
    WiFiClient client;
    client.setConnectionTimeout(budgetMs);
    if (!client.connect(IPAddress(resolver[0], resolver[1], resolver[2], resolver[3]), 53)) return false;
    bool ok = false;
    uint8_t prefix[2];
    if (client.write(query, queryLen) == queryLen && readExact(client, prefix, 2, deadline)) {
      const size_t len = (static_cast<size_t>(prefix[0]) << 8) | prefix[1];
      uint8_t message[kDnsMaxMessage];
      if (len >= 12 && len <= sizeof(message) && readExact(client, message, len, deadline)) {
        ok = parseDnsAnswer(message, len, id, host, out) == DnsParse::Ok;
      }
    }
    client.stop();
    return ok;
  }

  bool lastGood(const char* host, uint8_t out[4]) override {
    DnsMemory memory;
    if (!loadDnsMemory(fs_, memory) || strcasecmp(memory.host, host) != 0) return false;
    memcpy(out, memory.ip, 4);
    return true;
  }

 private:
  IFs& fs_;
};

#endif

}  // namespace

BorgesDns::BorgesDns(IFs& fs) : fs_(fs) {}

const Candidates& BorgesDns::resolve(const char* host, const uint32_t budgetMs, ICancel& cancel) {
  MillisClock clock;
#if defined(SIMULATOR)
  // curl resolves in the simulator: one placeholder keeps the caller's single path.
  class SimBackend : public IDnsBackend {
   public:
    bool getaddrinfo(const char*, uint32_t, uint8_t out[4]) override {
      const uint8_t ip[4] = {1, 0, 0, 1};
      memcpy(out, ip, 4);
      return true;
    }
    bool dnsOverTcp(const uint8_t[4], const char*, uint32_t, uint8_t[4]) override { return false; }
    bool lastGood(const char*, uint8_t[4]) override { return false; }
  } backend;
#else
  LiveBackend backend(fs_);
#endif
  return chain_.resolve(host, budgetMs, clock.monotonicMs(), backend, cancel, clock);
}

void BorgesDns::remember(const char* host, const uint8_t ip[4]) {
#if defined(SIMULATOR)
  (void)host;
  (void)ip;
#else
  DnsMemory memory;
  if (loadDnsMemory(fs_, memory) && strcasecmp(memory.host, host) == 0 && memcmp(memory.ip, ip, 4) == 0) return;
  if (strlen(host) >= sizeof(memory.host)) return;
  memset(&memory, 0, sizeof(memory));
  strcpy(memory.host, host);
  memcpy(memory.ip, ip, 4);
  saveDnsMemory(fs_, memory);
#endif
}

}  // namespace net
}  // namespace borges
