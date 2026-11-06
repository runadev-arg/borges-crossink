#pragma once

// request -> poll status -> claim, one non-blocking step at a time. The screen calls
// step() in its loop; the flow decides when a request is actually due, so it never
// polls faster than the server's `interval` and claims only once `approved`.

#include <cstddef>
#include <cstdint>

#include "Api.h"
#include "Outbox.h"
#include "Ports.h"
#include "Store.h"

namespace borges {

struct PairingConfig {
  const char* platform = "crossink-x4";
  const char* externalId = "";  // makeExternalId()
  const char* deviceName = "Xteink X4";
  const char* firmwareVersion = "";
  const char* clientVersion = "";
};

enum class PairingState : uint8_t {
  Idle,
  WaitingForApproval,  // Show userCode() and qrUrl().
  Done,                // Credential saved.
  Rejected,            // The account said no.
  Expired,             // Code expired. start() gets a new one.
  ClaimLost,           // The credential was issued but its response was lost. start() again.
  Failed,              // A request failed for a reason waiting will not fix.
};

class PairingFlow {
 public:
  PairingFlow(Store& store, Outbox& outbox, IFs& fs, IHttp& http, IClock& clock, IRandom& random, ICancel& cancel,
              const PairingConfig& config);

  // Requests a code, or resumes the one stored by a previous boot. False on failure;
  // the state and diagnostics say why. A rate limit keeps the flow Idle with retryAtMs().
  bool start();
  // Does at most one request, and only if one is due. Returns the current state.
  PairingState step();
  void cancelFlow();

  PairingState state() const { return state_; }
  const char* userCode() const { return store_.creds().pairing.userCode; }
  // https://<base>/devices/pair?code=XXXX-XXXX
  const char* qrUrl() const { return qrUrl_; }
  uint32_t intervalSec() const { return store_.creds().pairing.intervalSec; }
  uint64_t nextPollAtMs() const { return nextPollMs_; }
  bool rateLimited() const { return rateLimited_; }
  const Diagnostics& lastError() const { return error_; }

 private:
  bool requestCode();
  void pollStatus();
  void claim();
  void buildQrUrl();
  void clearPairing();
  void note(const ApiResult& result, const char* code);
  void scheduleRetry(const ApiResult& result);
  bool expiredLocally();
  void writeRequest(char* body, size_t cap, size_t& len);

  Store& store_;
  Outbox& outbox_;
  IFs& fs_;
  IClock& clock_;
  IRandom& random_;
  Api api_;
  PairingConfig config_;
  PairingState state_ = PairingState::Idle;
  uint64_t nextPollMs_ = 0;
  uint64_t expiresAtMs_ = 0;  // 0 = unknown in this boot.
  uint8_t errorRun_ = 0;
  bool rateLimited_ = false;
  char qrUrl_[192] = {0};
  Diagnostics error_;
};

// "x4-" + hash of the chip MAC: stable across boots, and the raw serial never leaves.
// out holds at least 20 bytes.
void makeExternalId(const uint8_t mac[6], char out[20]);

}  // namespace borges
