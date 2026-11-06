#include "PairingFlow.h"

#include <cstring>

#include "JsonStream.h"

namespace borges {

namespace {

constexpr const char* kRequestPath = "/api/devices/pairing/request";
constexpr const char* kStatusPath = "/api/devices/pairing/status";
constexpr const char* kClaimPath = "/api/devices/pairing/claim";
constexpr uint32_t kMinIntervalSec = 3;
constexpr uint32_t kMaxIntervalSec = 60;
constexpr uint32_t kBudgetMs = 15000;  // Per request: the flow itself has no wall limit but the code's expiry.

}  // namespace

void makeExternalId(const uint8_t mac[6], char out[20]) {
  const uint64_t h = fnv1a64(mac, 6, fnv1a64("borges-x4", 9));
  uint8_t raw[8];
  ByteWriter w(raw, sizeof(raw));
  w.u64(h);
  memcpy(out, "x4-", 3);
  hexEncode(raw, sizeof(raw), out + 3);
}

PairingFlow::PairingFlow(Store& store, Outbox& outbox, IFs& fs, IHttp& http, IClock& clock, IRandom& random,
                         ICancel& cancel, const PairingConfig& config)
    : store_(store),
      outbox_(outbox),
      fs_(fs),
      clock_(clock),
      random_(random),
      api_(fs, http, clock, cancel),
      config_(config) {}

void PairingFlow::buildQrUrl() {
  const Credentials& c = store_.creds();
  JsonWriter w(qrUrl_, sizeof(qrUrl_) - 1);
  w.raw(c.baseUrl);
  // Only a relative path from the server is trusted: the QR must not be redirectable
  // to another host by a response field.
  const char* path = c.pairing.verificationUrl;
  w.raw(path[0] == '/' && path[1] != '/' ? path : "/devices/pair");
  w.raw("?code=");
  w.raw(c.pairing.userCode);
  if (w.ok()) {
    qrUrl_[w.size()] = '\0';
  } else {
    qrUrl_[0] = '\0';
  }
}

void PairingFlow::clearPairing() {
  store_.creds().pairing = PendingPairing{};
  store_.saveCredentials();
  qrUrl_[0] = '\0';
  expiresAtMs_ = 0;
}

void PairingFlow::note(const ApiResult& r, const char* code) {
  error_.stage = SyncStage::Pairing;
  error_.httpStatus = static_cast<int16_t>(r.status);
  error_.transportStage = r.stage;
  error_.transportCode = r.transportCode;
  error_.atEpoch = clock_.wallNow().epochSec;
  copyText(error_.code, sizeof(error_.code), r.code[0] ? r.code : code);
  copyText(error_.detail, sizeof(error_.detail), r.message);
  copyText(error_.requestId, sizeof(error_.requestId), r.requestId);
}

void PairingFlow::scheduleRetry(const ApiResult& r) {
  // A failed poll waits longer each time, up to the slowest interval the server allows.
  uint32_t wait = store_.creds().pairing.intervalSec;
  for (uint8_t i = 0; i < errorRun_ && wait < kMaxIntervalSec; ++i) wait *= 2;
  if (wait > kMaxIntervalSec) wait = kMaxIntervalSec;
  if (r.hasRetryAfter && r.retryAfterSec > wait) wait = r.retryAfterSec;
  nextPollMs_ = clock_.monotonicMs() + static_cast<uint64_t>(wait) * 1000;
}

bool PairingFlow::start() {
  error_.clear();
  rateLimited_ = false;
  errorRun_ = 0;
  if (store_.creds().paired()) {
    state_ = PairingState::Done;
    return true;
  }
  api_.configure(store_.creds().baseUrl, "");
  if (store_.creds().pairing.active()) {
    // A code from a previous boot may still be alive. The status call says if not.
    buildQrUrl();
    state_ = PairingState::WaitingForApproval;
    nextPollMs_ = clock_.monotonicMs();
    return true;
  }
  return requestCode();
}

bool PairingFlow::requestCode() {
  Credentials& c = store_.creds();
  uint8_t raw[24];
  random_.fill(raw, sizeof(raw));
  char nonce[36];
  base64UrlEncode(raw, sizeof(raw), nonce);  // 32 chars of [A-Za-z0-9_-], as NONCE_RE wants.

  char body[640];
  JsonWriter w(body, sizeof(body));
  w.objectBegin();
  w.kv("device_nonce", nonce);
  w.kv("device_name", config_.deviceName);
  w.kv("platform", config_.platform);
  w.kv("external_id", config_.externalId);
  if (config_.firmwareVersion[0]) w.kv("firmware_version", config_.firmwareVersion);
  if (config_.clientVersion[0]) w.kv("client_version", config_.clientVersion);
  w.kvInt("protocol_version", 2);
  w.key("capabilities");
  w.objectBegin();
  w.kvBool("progress", true);
  w.kvBool("sessions", true);
  w.kvBool("page_stats", true);
  w.kvBool("annotations", true);
  w.kvBool("atomic_sd_outbox", true);
  w.objectEnd();
  w.objectEnd();
  if (!w.ok()) {
    state_ = PairingState::Failed;
    return false;
  }

  api_.beginSession(kBudgetMs);
  const ApiResult r = api_.call("POST", kRequestPath, reinterpret_cast<const uint8_t*>(body), w.size());
  if (r.kind != ApiKind::Ok) {
    note(r, r.kind == ApiKind::Transport ? "transport" : "pairing_request_failed");
    state_ = PairingState::Idle;
    if (r.status == 429) {
      rateLimited_ = true;
      scheduleRetry(r);
    } else if (r.kind == ApiKind::HttpError) {
      state_ = PairingState::Failed;
    }
    return false;
  }

  File f(fs_, kSpoolPath, IFs::Mode::Read);
  if (!f) {
    state_ = PairingState::Failed;
    return false;
  }
  FileByteSource source(f);
  JsonScanner scan(source);
  PendingPairing pending;
  copyText(pending.nonce, sizeof(pending.nonce), nonce);
  int64_t expiresIn = 0;
  int64_t interval = 5;
  char key[24];
  if (scan.enterObject()) {
    while (scan.nextMember(key, sizeof(key))) {
      if (strcmp(key, "request_id") == 0) {
        scan.readString(pending.requestId, sizeof(pending.requestId));
      } else if (strcmp(key, "user_code") == 0) {
        scan.readString(pending.userCode, sizeof(pending.userCode));
      } else if (strcmp(key, "verification_url") == 0) {
        scan.readString(pending.verificationUrl, sizeof(pending.verificationUrl));
      } else if (strcmp(key, "expires_in") == 0) {
        scan.readInt(expiresIn);
      } else if (strcmp(key, "interval") == 0) {
        scan.readInt(interval);
      } else {
        scan.skipValue();
      }
    }
  }
  if (scan.failed() || !isCanonicalUuid(pending.requestId) || strlen(pending.userCode) != 9) {
    ApiResult bad = r;
    note(bad, "malformed_response");
    state_ = PairingState::Failed;
    return false;
  }
  if (interval < kMinIntervalSec) interval = kMinIntervalSec;
  if (interval > kMaxIntervalSec) interval = kMaxIntervalSec;
  pending.intervalSec = static_cast<uint32_t>(interval);
  if (expiresIn > 0 && expiresIn < 86400) {
    expiresAtMs_ = clock_.monotonicMs() + static_cast<uint64_t>(expiresIn) * 1000;
    const WallTime now = clock_.wallNow();
    pending.expiresAtEpoch = now.precision == TimePrecision::Unknown ? 0 : now.epochSec + expiresIn;
  }
  c.pairing = pending;
  store_.saveCredentials();
  buildQrUrl();
  state_ = PairingState::WaitingForApproval;
  nextPollMs_ = clock_.monotonicMs() + static_cast<uint64_t>(pending.intervalSec) * 1000;
  return true;
}

bool PairingFlow::expiredLocally() { return expiresAtMs_ != 0 && clock_.monotonicMs() >= expiresAtMs_; }

PairingState PairingFlow::step() {
  if (state_ == PairingState::Idle && rateLimited_ && clock_.monotonicMs() >= nextPollMs_) {
    rateLimited_ = false;
    requestCode();
    return state_;
  }
  if (state_ != PairingState::WaitingForApproval) return state_;
  if (expiredLocally()) {
    clearPairing();
    state_ = PairingState::Expired;
    return state_;
  }
  if (clock_.monotonicMs() < nextPollMs_) return state_;
  pollStatus();
  return state_;
}

void PairingFlow::cancelFlow() {
  api_.endSession();
  state_ = PairingState::Idle;
}

void PairingFlow::pollStatus() {
  const PendingPairing& p = store_.creds().pairing;
  char body[192];
  JsonWriter w(body, sizeof(body));
  w.objectBegin();
  w.kv("request_id", p.requestId);
  w.kv("device_nonce", p.nonce);
  w.objectEnd();
  api_.beginSession(kBudgetMs);
  const ApiResult r = api_.call("POST", kStatusPath, reinterpret_cast<const uint8_t*>(body), w.size());
  if (r.kind != ApiKind::Ok) {
    note(r, r.kind == ApiKind::Transport ? "transport" : "pairing_status_failed");
    if (r.status == 404 || r.status == 410) {
      clearPairing();
      state_ = PairingState::Expired;
      return;
    }
    rateLimited_ = r.status == 429;
    if (r.kind != ApiKind::Cancelled) {
      if (errorRun_ < 8) ++errorRun_;
      scheduleRetry(r);
    }
    return;
  }

  File f(fs_, kSpoolPath, IFs::Mode::Read);
  char status[20] = {0};
  if (f) {
    FileByteSource source(f);
    JsonScanner scan(source);
    char key[24];
    if (scan.enterObject()) {
      while (scan.nextMember(key, sizeof(key))) {
        if (strcmp(key, "status") == 0) {
          scan.readString(status, sizeof(status));
        } else {
          scan.skipValue();
        }
      }
    }
    if (scan.failed()) status[0] = '\0';
  }
  f.close();
  rateLimited_ = false;
  errorRun_ = 0;
  if (strcmp(status, "approved") == 0) {
    claim();
  } else if (strcmp(status, "pending") == 0) {
    scheduleRetry(ApiResult{});
  } else if (strcmp(status, "rejected") == 0) {
    clearPairing();
    state_ = PairingState::Rejected;
  } else if (strcmp(status, "expired") == 0) {
    clearPairing();
    state_ = PairingState::Expired;
  } else if (strcmp(status, "claimed") == 0) {
    clearPairing();
    state_ = PairingState::ClaimLost;
  } else {
    // Not an answer. Treat like a failed poll: keep waiting, a little slower.
    note(r, "malformed_response");
    if (errorRun_ < 8) ++errorRun_;
    scheduleRetry(ApiResult{});
  }
}

void PairingFlow::claim() {
  const PendingPairing& p = store_.creds().pairing;
  char body[192];
  JsonWriter w(body, sizeof(body));
  w.objectBegin();
  w.kv("request_id", p.requestId);
  w.kv("device_nonce", p.nonce);
  w.objectEnd();
  const ApiResult r = api_.callOnce("POST", kClaimPath, reinterpret_cast<const uint8_t*>(body), w.size());
  if (r.kind != ApiKind::Ok) {
    scrubFile(fs_, kSpoolPath);  // A cut answer can still hold the token.
    note(r, r.kind == ApiKind::Transport ? "transport" : "pairing_claim_failed");
    if (strcmp(r.code, "authorization_pending") == 0) {
      scheduleRetry(r);
    } else if (strcmp(r.code, "pairing_rejected") == 0) {
      clearPairing();
      state_ = PairingState::Rejected;
    } else if (strcmp(r.code, "pairing_expired") == 0 || r.status == 404) {
      clearPairing();
      state_ = PairingState::Expired;
    } else if (strcmp(r.code, "pairing_already_claimed") == 0) {
      clearPairing();
      state_ = PairingState::ClaimLost;
    } else {
      rateLimited_ = r.status == 429;
      if (r.kind != ApiKind::Cancelled) {
        if (errorRun_ < 8) ++errorRun_;
        scheduleRetry(r);
      }
    }
    return;
  }

  File f(fs_, kSpoolPath, IFs::Mode::Read);
  char token[sizeof(store_.creds().token)] = {0};
  char deviceId[sizeof(store_.creds().deviceId)] = {0};
  char credentialDevice[sizeof(store_.creds().deviceId)] = {0};
  char accountId[sizeof(store_.creds().accountId)] = {0};
  char accountName[sizeof(store_.creds().accountName)] = {0};
  int64_t expires = 0;
  int64_t renew = 0;
  bool tokenCut = false;
  bool failed = !f;
  if (f) {
    FileByteSource source(f);
    JsonScanner scan(source);
    char key[24];
    if (scan.enterObject()) {
      while (scan.nextMember(key, sizeof(key))) {
        const bool isCredential = strcmp(key, "credential") == 0;
        const bool isDevice = strcmp(key, "device") == 0;
        const bool isAccount = strcmp(key, "account") == 0;
        if ((isCredential || isDevice || isAccount) && scan.enterObject()) {
          while (scan.nextMember(key, sizeof(key))) {
            if (isCredential && strcmp(key, "token") == 0) {
              scan.readString(token, sizeof(token), &tokenCut);
            } else if (isCredential && strcmp(key, "username") == 0) {
              scan.readString(credentialDevice, sizeof(credentialDevice));
            } else if (isCredential && strcmp(key, "expires_at") == 0) {
              char text[40];
              if (!scan.readString(text, sizeof(text)) || !parseRfc3339(text, expires)) expires = 0;
            } else if (isCredential && strcmp(key, "renew_after") == 0) {
              char text[40];
              if (!scan.readString(text, sizeof(text)) || !parseRfc3339(text, renew)) renew = 0;
            } else if (isDevice && strcmp(key, "id") == 0) {
              scan.readString(deviceId, sizeof(deviceId));
            } else if (isAccount && strcmp(key, "id") == 0) {
              scan.readString(accountId, sizeof(accountId));
            } else if (isAccount && strcmp(key, "username") == 0) {
              scan.readString(accountName, sizeof(accountName));
            } else {
              scan.skipValue();
            }
          }
        } else {
          scan.skipValue();
        }
      }
    }
    failed = scan.failed();
  }
  f.close();
  // The token must not outlive this function anywhere but in the sealed record.
  scrubFile(fs_, kSpoolPath);
  if (!deviceId[0]) copyText(deviceId, sizeof(deviceId), credentialDevice);
  if (failed || !token[0] || tokenCut || !deviceId[0]) {
    note(r, "malformed_response");
    state_ = PairingState::ClaimLost;  // A claim works once: only a new code can recover.
    clearPairing();
    return;
  }
  if (!store_.adoptAccount(accountId, accountName, outbox_)) {
    ApiResult none;
    note(none, "account_switch_failed");
    state_ = PairingState::Failed;
    return;
  }
  Credentials& c = store_.creds();
  copyText(c.deviceId, sizeof(c.deviceId), deviceId);
  copyText(c.token, sizeof(c.token), token);
  memset(token, 0, sizeof(token));
  c.expiresAtEpoch = expires;
  c.renewAfterEpoch = renew;
  c.pairing = PendingPairing{};
  store_.state().needsRelink = false;
  store_.state().failures = 0;
  store_.state().nextAttemptEpoch = 0;
  if (!store_.saveCredentials()) {
    ApiResult none;
    note(none, "credential_write_failed");
    state_ = PairingState::Failed;
    return;
  }
  store_.saveState();
  error_.clear();
  state_ = PairingState::Done;
}

}  // namespace borges
