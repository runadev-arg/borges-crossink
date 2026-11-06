#include "BorgesHttp.h"

#include <Arduino.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "BorgesClock.h"
#include "BorgesDns.h"
#include "BorgesTrustAnchors.h"
#include "net/NetFailure.h"
#include "net/NetRecords.h"

#if defined(FREEINK_NET_WOLFSSL)
#include <wolfssl/error-ssl.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/tfm.h>
static_assert(borges::net::wolfcode::kMemory == MEMORY_E);
static_assert(borges::net::wolfcode::kMathMemory == MP_MEM);
static_assert(borges::net::wolfcode::kBeforeDate == ASN_BEFORE_DATE_E);
static_assert(borges::net::wolfcode::kAfterDate == ASN_AFTER_DATE_E);
static_assert(borges::net::wolfcode::kSigConfirm == ASN_SIG_CONFIRM_E);
static_assert(borges::net::wolfcode::kNoSigner == ASN_NO_SIGNER_E);
static_assert(borges::net::wolfcode::kHashType == HASH_TYPE_E);
static_assert(borges::net::wolfcode::kNameMismatch == DOMAIN_NAME_MISMATCH);
static_assert(borges::net::wolfcode::kVerifyCert == VERIFY_CERT_ERROR);
#endif

namespace borges {
namespace net {
namespace {

constexpr uint32_t kIoTimeoutMs = 10000;
constexpr uint32_t kConnectTimeoutMs = 3500;
constexpr uint32_t kDnsBudgetMs = 6000;
constexpr uint32_t kClockRepairBudgetMs = 8000;

using Fail = freeink::SecureClient::FailStage;

// "https://host[:port]/..." -> host. False when it does not fit or does not parse.
bool hostOf(const char* url, char* out, const size_t cap) {
  const char* start = strstr(url, "://");
  if (start == nullptr) return false;
  start += 3;
  size_t n = strcspn(start, ":/?#");
  if (n == 0 || n >= cap || memchr(start, '@', n) != nullptr) return false;  // No userinfo: it is another host.
  memcpy(out, start, n);
  out[n] = '\0';
  return true;
}

void copyHeader(const freeink::SecureHttpClient& http, const char* name, char* out, const size_t cap) {
  const std::string value = http.getHeader(name);
  strncpy(out, value.c_str(), cap - 1);
  out[cap - 1] = '\0';
}

}  // namespace

namespace {
constexpr uint32_t kMinBlockForRequest = 4 * 1024;
}  // namespace

BorgesHttp::BorgesHttp(IFs& fs, BorgesDns& dns, BorgesClock* clock, const char* userAgent)
    : fs_(fs), dns_(dns), clock_(clock), userAgent_(userAgent) {}

void BorgesHttp::disconnect() {
  http_.end();
  clockRepairUsed_ = false;
}

void BorgesHttp::perform(const HttpRequest& request, IHttpSink& sink, ICancel& abort, HttpResponse& out) {
  out = HttpResponse{};

  // The handshake's own buffers are gone by the time it reports, so the heap it started on counts.
  const uint32_t heapAtStart = ESP.getFreeHeap();
  const auto finish = [&](const TransportStage stage, const int32_t rawCode) {
    const uint32_t heapNow = ESP.getFreeHeap();
    const int32_t code = heapAwareCode(stage, rawCode, heapNow < heapAtStart ? heapNow : heapAtStart);
    out.stage = stage;
    out.transportCode = code;
    if (stage == TransportStage::None || stage == TransportStage::Aborted) return;
    NetDiag diag;
    diag.stage = stage;
    diag.code = code;
    const WallTime now = clock_ ? clock_->wallNow() : WallTime{};
    diag.atEpoch = now.epochSec;
    strncpy(diag.failure, failureCode(stage, code, now.precision), sizeof(diag.failure) - 1);
    saveNetDiag(fs_, diag);
  };

  char host[64];
  if (!hostOf(request.url, host, sizeof(host))) return finish(TransportStage::Connect, kCodeBadUrl);
  if (abort.cancelled()) return finish(TransportStage::Aborted, 0);

  // The URL, the headers and the response strings below abort if they cannot allocate.
  if (ESP.getMaxAllocHeap() < kMinBlockForRequest) return finish(TransportStage::Connect, kCodeLowMemory);

  http_.setTrustAnchors(trustAnchors(), kTrustAnchorCount);
  http_.setUserAgent(userAgent_);
  http_.setTimeout(kIoTimeoutMs);
  http_.setConnectTimeout(kConnectTimeoutMs);
  if (!http_.begin(std::string(request.url))) return finish(TransportStage::Connect, kCodeBadUrl);
  for (uint8_t i = 0; i < request.headerCount; ++i) {
    http_.addHeader(request.headers[i].name, request.headers[i].value);
  }

  const bool fresh = !http_.hasLiveConnection();
  const Candidates* candidates = nullptr;
  if (fresh) {
    if (!heapAllowsHandshake(ESP.getFreeHeap(), ESP.getMaxAllocHeap())) {
      return finish(TransportStage::Connect, kCodeLowMemory);
    }
    candidates = &dns_.resolve(host, kDnsBudgetMs, abort);
    if (abort.cancelled()) return finish(TransportStage::Aborted, 0);
    if (candidates->count == 0) return finish(TransportStage::Dns, kCodeNoAddress);
  }

  bool statusSent = false;
  const auto onData = [&](const uint8_t* data, const size_t len) {
    if (!statusSent) {
      statusSent = true;
      sink.onStatus(http_.getStatus());
    }
    return sink.onBody(data, len);
  };
  const auto shouldAbort = [&abort]() { return abort.cancelled(); };

  uint8_t next = 0;
  for (;;) {
    if (fresh) {
      if (next >= candidates->count) return finish(TransportStage::Connect, kCodeTcp);
      const uint8_t* ip = candidates->items[next].ip;
      http_.setServerAddress(IPAddress(ip[0], ip[1], ip[2], ip[3]));
    }
    const int status = http_.sendRequest(request.method, request.body, request.bodyLen, onData, shouldAbort);

    if (status > 0) {
      if (fresh) dns_.remember(host, candidates->items[next].ip);
      if (!statusSent) {
        statusSent = true;
        sink.onStatus(status);
      }
      out.status = status;
      out.complete = http_.responseComplete();
      copyHeader(http_, "x-request-id", out.requestId, sizeof(out.requestId));
      copyHeader(http_, "etag", out.etag, sizeof(out.etag));
      copyHeader(http_, "x-content-sha256", out.contentSha256, sizeof(out.contentSha256));
      copyHeader(http_, "x-koreader-partial-md5", out.koreaderMd5, sizeof(out.koreaderMd5));
      const std::string retryAfter = http_.getHeader("retry-after");
      if (!retryAfter.empty() && retryAfter[0] >= '0' && retryAfter[0] <= '9') {
        out.hasRetryAfter = true;
        out.retryAfterSec = static_cast<uint32_t>(strtoul(retryAfter.c_str(), nullptr, 10));
      }
      if (out.complete) return;
      if (http_.aborted() || http_.callbackAborted()) return finish(TransportStage::Aborted, 0);
      return finish(TransportStage::ReadBody, kCodeLost);
    }

    if (http_.aborted() || abort.cancelled()) return finish(TransportStage::Aborted, 0);
    switch (http_.lastStage()) {
      case decltype(http_)::Stage::Send:
        return finish(TransportStage::Send, kCodeLost);
      case decltype(http_)::Stage::Headers:
        return finish(TransportStage::ReadHeaders, kCodeLost);
      case decltype(http_)::Stage::Body:
        return finish(TransportStage::ReadBody, kCodeLost);
      default:
        break;
    }

    const auto& secure = http_.secureClient();
    const int code = secure.lastFailCode();
    switch (secure.lastFailStage()) {
      case Fail::Tcp:
      case Fail::None:
        // Another address may answer. Everything below would fail the same way there.
        // A reused connection that could not reconnect has no list to walk: the next
        // perform() starts fresh, with DNS.
        if (!fresh) return finish(TransportStage::Connect, kCodeTcp);
        ++next;
        continue;
      case Fail::Alloc:
        return finish(TransportStage::Tls, kCodeLowMemory);
      case Fail::TrustLoad:
        return finish(TransportStage::Tls, kCodeAnchorLoad);
      case Fail::Timeout:
        return finish(TransportStage::Tls, kCodeTimeout);
      case Fail::Aborted:
        return finish(TransportStage::Aborted, 0);
      case Fail::Handshake:
        break;
    }
    if (isClockFailure(TransportStage::Tls, code) && clock_ != nullptr && !clockRepairUsed_ &&
        clock_->wallNow().precision != TimePrecision::Exact) {
      clockRepairUsed_ = true;
      if (clock_->repair(kClockRepairBudgetMs, abort, code == wolfcode::kAfterDate)) continue;
    }
    return finish(TransportStage::Tls, code);
  }
}

}  // namespace net
}  // namespace borges
