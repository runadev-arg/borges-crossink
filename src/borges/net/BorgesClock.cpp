#include "BorgesClock.h"

#include <Arduino.h>
#include <sys/time.h>
#if !defined(SIMULATOR)
#include <SecureHttpClient.h>
#include <WiFiClient.h>
#endif

#include <cstring>
#include <ctime>
#include <string>

#include "BorgesDns.h"
#include "net/ClockChain.h"
#include "net/HttpDate.h"
#include "net/NetFailure.h"

#if !defined(SIMULATOR) && !defined(BORGES_NET_PROBE)
#include <esp_sntp.h>
#include <esp_system.h>
#include <esp_timer.h>
#endif

#if !defined(RTC_NOINIT_ATTR)
#define RTC_NOINIT_ATTR
#endif

#if defined(BORGES_NET_PROBE)
// The probe moves the clock wolfSSL sees.
int64_t borgesProbeNow();
void borgesProbeSetNow(int64_t epoch);
#endif

namespace borges {
namespace net {
namespace {

int64_t readSystemClock() {
#if defined(BORGES_NET_PROBE)
  return borgesProbeNow();
#else
  return static_cast<int64_t>(time(nullptr));
#endif
}

void writeSystemClock(const int64_t epoch) {
#if defined(BORGES_NET_PROBE)
  borgesProbeSetNow(epoch);
#elif defined(SIMULATOR)
  (void)epoch;  // The host's clock is not ours to move.
#else
  const timeval tv{static_cast<time_t>(epoch), 0};
  settimeofday(&tv, nullptr);
#endif
}

// Survives esp_restart() and deep sleep; garbage after a power-on, hence the check.
struct RetainedLabel {
  uint32_t magic;
  uint32_t label;
  uint32_t inverse;
};
RTC_NOINIT_ATTR RetainedLabel sRetained;
constexpr uint32_t kMagic = 0x42434C4B;  // "BCLK"

TimePrecision sLabel = TimePrecision::Unknown;
bool sLabeledThisBoot = false;

TimePrecision readRetained() {
  if (sRetained.magic != kMagic || sRetained.label != ~sRetained.inverse) return TimePrecision::Unknown;
  if (sRetained.label > static_cast<uint32_t>(TimePrecision::Exact)) return TimePrecision::Unknown;
  return static_cast<TimePrecision>(sRetained.label);
}

void writeLabel(const TimePrecision label) {
  sLabel = label;
  sRetained.magic = kMagic;
  sRetained.label = static_cast<uint32_t>(label);
  sRetained.inverse = ~sRetained.label;
}

ResetKind resetKind() {
#if defined(SIMULATOR) || defined(BORGES_NET_PROBE)
  return ResetKind::PowerOn;
#else
  switch (esp_reset_reason()) {
    case ESP_RST_SW:
      return ResetKind::SoftwareRestart;
    case ESP_RST_DEEPSLEEP:
      return ResetKind::DeepSleepWake;
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return ResetKind::Crash;
    default:
      return ResetKind::PowerOn;
  }
#endif
}

#if !defined(SIMULATOR)

// A captive portal must speak HTTP to set the clock: the status line is checked first.
// A long header (Cloudflare's report-to) is truncated, not fatal, so that it cannot hide
// the Date behind it. The byte cap is what bounds a hostile peer.
bool httpDate(const uint8_t ip[4], const char* host, const uint32_t budgetMs, ICancel& cancel, int64_t& epoch) {
  constexpr size_t kMaxHeaderBytes = 4096;
  const uint32_t deadline = millis() + budgetMs;
  WiFiClient client;
  client.setConnectionTimeout(budgetMs);
  if (!client.connect(IPAddress(ip[0], ip[1], ip[2], ip[3]), 80)) return false;

  std::string request = "HEAD / HTTP/1.1\r\nHost: ";
  request += host;
  request += "\r\nUser-Agent: Borges\r\nConnection: close\r\n\r\n";
  bool found = false;
  if (client.write(reinterpret_cast<const uint8_t*>(request.data()), request.size()) == request.size()) {
    char line[64];
    size_t lineLen = 0;
    size_t total = 0;
    bool sawStatus = false;
    while (!found && total < kMaxHeaderBytes && static_cast<int32_t>(millis() - deadline) < 0 && !cancel.cancelled()) {
      const int c = client.read();
      if (c < 0) {
        if (!client.connected() && client.available() == 0) break;
        delay(5);
        continue;
      }
      ++total;
      if (c != '\n') {
        if (lineLen < sizeof(line) - 1) line[lineLen++] = static_cast<char>(c);
        continue;
      }
      line[lineLen] = '\0';
      if (!sawStatus) {
        if (strncmp(line, "HTTP/1.", 7) != 0) break;
        sawStatus = true;
      } else if (lineLen <= 1) {
        break;  // End of headers.
      } else if (strncasecmp(line, "date:", 5) == 0) {
        found = parseHttpDate(line + 5, lineLen - 5, epoch);
        break;
      }
      lineLen = 0;
    }
  }
  client.stop();
  return found;
}

// Certificate verification is off for this one read and nowhere else: verification
// needs the clock this is trying to find. The value is bounded before anyone believes
// it, and it earns only the approximate label. This client dies here and carries nothing.
// GET with a sink that refuses the first byte: HEAD has a Content-Length with no body
// behind it, and the body reader would wait for bytes that never come.
bool httpsDate(const uint8_t ip[4], const char* host, const uint32_t budgetMs, ICancel& cancel, int64_t& epoch) {
  if (!heapAllowsHandshake(ESP.getFreeHeap(), ESP.getMaxAllocHeap())) return false;
  freeink::SecureHttpClient http;
  http.setInsecure();
  http.setTimeout(budgetMs);
  http.setReuse(false);
  http.setUserAgent("Borges");
  if (!http.begin(std::string("https://") + host + "/")) return false;
  http.setServerAddress(IPAddress(ip[0], ip[1], ip[2], ip[3]));
  const int status = http.sendRequest(
      "GET", nullptr, 0, [](const uint8_t*, size_t) { return false; }, [&cancel]() { return cancel.cancelled(); });
  bool ok = false;
  if (status > 0) {
    const std::string date = http.getHeader("date");
    ok = parseHttpDate(date.data(), date.size(), epoch);
  }
  http.end();
  return ok;
}

#endif  // !SIMULATOR

bool sntp(const uint32_t budgetMs, ICancel& cancel, int64_t& epoch) {
#if defined(SIMULATOR) || defined(BORGES_NET_PROBE)
  (void)budgetMs;
  (void)cancel;
  (void)epoch;
  return false;
#else
  const int64_t before = readSystemClock();
  esp_sntp_stop();
  sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "time.cloudflare.com");
  esp_sntp_setservername(1, "pool.ntp.org");
  esp_sntp_setservername(2, "time.google.com");
  esp_sntp_init();
  const uint32_t start = millis();
  bool synced = false;
  while (millis() - start < budgetMs && !cancel.cancelled()) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      synced = true;
      break;
    }
    delay(50);
  }
  // Stopped before the value is read and on every path: a late answer must not move a
  // clock that something else has since set.
  esp_sntp_stop();
  if (synced) {
    // lwIP has already set the system clock. It goes back until repairClock() has judged
    // the answer: this is the one source that does not wait to be believed.
    epoch = readSystemClock();
    writeSystemClock(before + static_cast<int64_t>((millis() - start) / 1000));
  }
  return synced;
#endif
}

class Sources : public IClockSources {
 public:
  Sources(BorgesDns& dns, const char* host) : dns_(dns), host_(host) {}

  bool read(const ClockSource source, const uint32_t budgetMs, ICancel& cancel, int64_t& epoch) override {
    if (source == ClockSource::Sntp) return sntp(budgetMs, cancel, epoch);
#if defined(SIMULATOR)
    return false;  // begin() labels the host clock exact, so nothing ever asks.
#else
    const uint32_t start = millis();
    const Candidates& candidates = dns_.resolve(host_, budgetMs / 2, cancel);
    for (uint8_t i = 0; i < candidates.count; ++i) {
      const uint32_t spent = millis() - start;
      if (spent >= budgetMs || cancel.cancelled()) return false;
      const uint32_t left = budgetMs - spent;
      const bool ok = source == ClockSource::HttpDate ? httpDate(candidates.items[i].ip, host_, left, cancel, epoch)
                                                      : httpsDate(candidates.items[i].ip, host_, left, cancel, epoch);
      if (ok) return true;
    }
    return false;
#endif
  }

 private:
  BorgesDns& dns_;
  const char* host_;
};

}  // namespace

BorgesClock::BorgesClock(BorgesDns& dns, const char* host) : dns_(dns), host_(host) {}

void BorgesClock::begin(const int64_t storedAnchorEpoch) {
  anchorEpoch_ = storedAnchorEpoch;
  // The reset reason describes the boot, not this call: a later session in the same
  // boot must not take back what an earlier one established.
#if !defined(BORGES_NET_PROBE)
  if (sLabeledThisBoot) return;
#endif
  sLabeledThisBoot = true;
#if defined(SIMULATOR)
  writeLabel(TimePrecision::Exact);  // The host clock.
#else
  writeLabel(labelAtBoot(resetKind(), readRetained(), readSystemClock(), storedAnchorEpoch));
#endif
}

uint64_t BorgesClock::monotonicMs() {
#if defined(SIMULATOR) || defined(BORGES_NET_PROBE)
  return millis();
#else
  return static_cast<uint64_t>(esp_timer_get_time() / 1000);
#endif
}

WallTime BorgesClock::wallNow() {
  const int64_t epoch = readSystemClock();
  if (!epochInBounds(epoch)) return WallTime{epoch, TimePrecision::Unknown};
  return WallTime{epoch, sLabel};
}

void BorgesClock::setWall(const int64_t epochSec, const TimePrecision precision) {
  writeSystemClock(epochSec);
  writeLabel(precision);
}

ClockTrust BorgesClock::trust() const {
  ClockTrust t;
  t.anchorEpoch = anchorEpoch_;
  t.buildEpoch = epochOfBuildDate(__DATE__);
  t.allowLongGap = allowLongGap_;
  return t;
}

bool BorgesClock::ensure(const uint32_t budgetMs, ICancel& cancel) {
  Sources sources(dns_, host_);
  return ensureClock(*this, trust(), budgetMs, sources, cancel);
}

bool BorgesClock::repair(const uint32_t budgetMs, ICancel& cancel) {
  Sources sources(dns_, host_);
  return repairClock(*this, trust(), budgetMs, sources, cancel);
}

}  // namespace net
}  // namespace borges
