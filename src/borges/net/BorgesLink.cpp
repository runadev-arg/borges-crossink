#include "BorgesLink.h"

#include <cstdlib>
#include <cstring>
#include <ctime>

#include "net/NetRecords.h"

#if !defined(SIMULATOR)
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "WifiCredentialStore.h"
#endif

namespace borges {
namespace net {

BorgesLink::BorgesLink(IFs& fs) : fs_(fs) {}

BorgesLink::~BorgesLink() { disconnect(); }

#if defined(SIMULATOR)

// The simulator has the host's network. BORGES_SIM_NO_WIFI makes it behave as out of range and
// BORGES_SIM_WIFI_AUTH as a password refused, until the picker has joined something: its result is
// what a person choosing another network would leave behind. BORGES_SIM_NO_SAVED_WIFI has no link
// until then either, or the first-time picker could not be reached.
namespace {
bool simPickerJoined(IFs& fs) { return fs.exists("/.crosspoint/borges/sim-picker-joined"); }
bool simEnv(const char* name) {
  const char* value = getenv(name);
  return value != nullptr && value[0] != '\0';
}
}  // namespace

LinkResult BorgesLink::connect(uint32_t, ICancel& cancel) {
  if (cancel.cancelled()) return LinkResult::Cancelled;
  if (simPickerJoined(fs_)) return LinkResult::Connected;
  if (simEnv("BORGES_SIM_WIFI_AUTH")) return LinkResult::AuthFailed;
  if (simEnv("BORGES_SIM_NO_SAVED_WIFI")) return LinkResult::NoSavedNetworks;
  return simEnv("BORGES_SIM_NO_WIFI") ? LinkResult::NotFound : LinkResult::Connected;
}
void BorgesLink::disconnect() {}
bool BorgesLink::connected() const {
  if (simPickerJoined(fs_)) return true;
  return !simEnv("BORGES_SIM_NO_WIFI") && !simEnv("BORGES_SIM_WIFI_AUTH") && !simEnv("BORGES_SIM_NO_SAVED_WIFI");
}

#else

namespace {

constexpr uint32_t kHintShareOfBudgetDivisor = 2;
constexpr uint32_t kHintMaxMs = 4000;
constexpr uint32_t kScanMaxMs = 5000;
constexpr uint32_t kScanPerChannelMs = 120;
constexpr uint32_t kJoinMaxMs = 8000;
constexpr int kMaxJoinAttempts = 3;

uint32_t minOf(const uint32_t a, const uint32_t b) { return a < b ? a : b; }

LinkResult waitForJoin(const uint32_t budgetMs, ICancel& cancel) {
  const uint32_t start = millis();
  while (millis() - start < budgetMs) {
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) return LinkResult::Connected;
    if (status == WL_NO_SSID_AVAIL) return LinkResult::NotFound;
    if (status == WL_CONNECT_FAILED) return LinkResult::AuthFailed;
    if (cancel.cancelled()) return LinkResult::Cancelled;
    delay(50);
  }
  return LinkResult::Timeout;
}

// The BSSID and channel, when given, skip the scan: that is the ~1-2 s path.
LinkResult join(const WifiCredential& credential, const uint8_t channel, const uint8_t* bssid, const uint32_t budgetMs,
                ICancel& cancel) {
  WiFi.disconnect(false, false);
  const char* password = credential.password.empty() ? nullptr : credential.password.c_str();
  WiFi.begin(credential.ssid.c_str(), password, channel, bssid, true);
  return waitForJoin(budgetMs, cancel);
}

}  // namespace

LinkResult BorgesLink::connect(const uint32_t budgetMs, ICancel& cancel) {
  const uint32_t start = millis();
  const auto left = [&]() -> uint32_t {
    const uint32_t spent = millis() - start;
    return spent >= budgetMs ? 0 : budgetMs - spent;
  };

  LinkResult result = LinkResult::NotFound;
  const auto done = [&](const LinkResult r) {
    if (r == LinkResult::Connected) {
      esp_wifi_set_ps(WIFI_PS_NONE);
      LinkHint now;
      strncpy(now.ssid, WiFi.SSID().c_str(), sizeof(now.ssid) - 1);
      const uint8_t* bssid = WiFi.BSSID();
      if (bssid != nullptr) memcpy(now.bssid, bssid, 6);
      now.channel = static_cast<uint8_t>(WiFi.channel());
      LinkHint saved;
      const bool same = loadLinkHint(fs_, saved) && strcmp(saved.ssid, now.ssid) == 0 &&
                        memcmp(saved.bssid, now.bssid, 6) == 0 && saved.channel == now.channel;
      if (!same) {
        saveLinkHint(fs_, now);
        WIFI_STORE.setLastConnectedSsid(now.ssid);
      }
      return r;
    }
    disconnect();
    if (r != LinkResult::Cancelled) {
      NetDiag diag;
      diag.stage = TransportStage::None;
      diag.code = static_cast<int32_t>(r);
      const time_t wall = time(nullptr);
      diag.atEpoch = static_cast<int64_t>(wall);
      strncpy(diag.failure, linkResultCode(r), sizeof(diag.failure) - 1);
      saveNetDiag(fs_, diag);
    }
    return r;
  };

  if (cancel.cancelled()) return done(LinkResult::Cancelled);
  if (WIFI_STORE.getCredentialCount() == 0) return done(LinkResult::NoSavedNetworks);

  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  if (!WiFi.mode(WIFI_STA)) return done(LinkResult::RadioError);
  esp_wifi_set_ps(WIFI_PS_NONE);

  // 1. The last network, straight to its access point.
  LinkHint hint;
  if (loadLinkHint(fs_, hint) && hint.usable()) {
    const auto credential = WIFI_STORE.findCredential(hint.ssid);
    if (credential) {
      result =
          join(*credential, hint.channel, hint.bssid, minOf(kHintMaxMs, left() / kHintShareOfBudgetDivisor), cancel);
      if (result == LinkResult::Connected || result == LinkResult::Cancelled) return done(result);
    }
  }

  // 2. A scan of what is in range, strongest saved network first.
  if (left() == 0) return done(LinkResult::Timeout);
  WiFi.disconnect(false, false);
  if (WiFi.scanNetworks(true, false, false, kScanPerChannelMs) != WIFI_SCAN_RUNNING)
    return done(LinkResult::RadioError);
  const uint32_t scanStart = millis();
  int16_t found = WIFI_SCAN_RUNNING;
  while ((found = WiFi.scanComplete()) == WIFI_SCAN_RUNNING) {
    if (cancel.cancelled()) {
      WiFi.scanDelete();
      return done(LinkResult::Cancelled);
    }
    if (millis() - scanStart >= minOf(kScanMaxMs, left())) {
      WiFi.scanDelete();
      return done(LinkResult::Timeout);
    }
    delay(50);
  }
  if (found < 0) return done(LinkResult::RadioError);

  bool tried[32] = {false};
  const int16_t visible = found < 32 ? found : 32;
  result = LinkResult::NotFound;
  for (int attempt = 0; attempt < kMaxJoinAttempts && left() > 0; ++attempt) {
    int16_t best = -1;
    int32_t bestRssi = INT32_MIN;
    for (int16_t i = 0; i < visible; ++i) {
      if (tried[i] || WiFi.RSSI(i) <= bestRssi) continue;
      if (!WIFI_STORE.hasSavedCredential(WiFi.SSID(i).c_str())) continue;
      best = i;
      bestRssi = WiFi.RSSI(i);
    }
    if (best < 0) break;
    tried[best] = true;
    const auto credential = WIFI_STORE.findCredential(WiFi.SSID(best).c_str());
    if (!credential) continue;
    uint8_t bssid[6];
    memcpy(bssid, WiFi.BSSID(best), 6);
    const auto channel = static_cast<uint8_t>(WiFi.channel(best));
    const LinkResult joined = join(*credential, channel, bssid, minOf(kJoinMaxMs, left()), cancel);
    if (joined == LinkResult::Connected || joined == LinkResult::Cancelled) {
      WiFi.scanDelete();
      return done(joined);
    }
    // The most telling failure wins: a refused password beats a timeout.
    if (result != LinkResult::AuthFailed) result = joined;
  }
  WiFi.scanDelete();
  return done(left() == 0 && result == LinkResult::NotFound ? LinkResult::Timeout : result);
}

void BorgesLink::disconnect() {
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
}

bool BorgesLink::connected() const { return WiFi.status() == WL_CONNECTED; }

#endif

}  // namespace net
}  // namespace borges
