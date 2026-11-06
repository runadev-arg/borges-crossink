#pragma once

// Turns (stage, code) into the plain word the Borges screen shows. The codes are
// wolfSSL's where wolfSSL produced them; the values are repeated here because this
// header cannot include wolfSSL, and BorgesHttp.cpp static_asserts that they match.

#include <cstddef>
#include <cstdint>

#include "Ports.h"

namespace borges {
namespace net {

namespace wolfcode {
constexpr int32_t kMemory = -125;        // MEMORY_E
constexpr int32_t kMathMemory = -2;      // MP_MEM, fast-math out of heap
constexpr int32_t kBeforeDate = -150;    // ASN_BEFORE_DATE_E
constexpr int32_t kAfterDate = -151;     // ASN_AFTER_DATE_E
constexpr int32_t kSigConfirm = -155;    // ASN_SIG_CONFIRM_E
constexpr int32_t kNoSigner = -188;      // ASN_NO_SIGNER_E
constexpr int32_t kHashType = -232;      // HASH_TYPE_E
constexpr int32_t kNameMismatch = -322;  // DOMAIN_NAME_MISMATCH
constexpr int32_t kVerifyCert = -329;    // VERIFY_CERT_ERROR
}  // namespace wolfcode

// Failures that come from this layer. Far from any wolfSSL or socket value.
constexpr int32_t kCodeLowMemory = -9001;      // The heap gate refused the handshake.
constexpr int32_t kCodeNoAddress = -9002;      // The DNS chain produced nothing.
constexpr int32_t kCodeTcp = -9003;            // No candidate accepted the connection.
constexpr int32_t kCodeTimeout = -9004;        // The handshake ran out of time.
constexpr int32_t kCodeAnchorLoad = -9005;     // A trust anchor failed to load.
constexpr int32_t kCodeBadUrl = -9006;         // The URL did not parse.
constexpr int32_t kCodeLost = -9007;           // The connection died mid-exchange.
constexpr int32_t kCodeCaptivePortal = -9008;  // The network wants a login on a web page first.

// A failure a network holding everything back until a web login would also cause. Only these are
// worth a plain HTTP question: a date or a cipher says nothing about who is answering.
bool couldBeCaptivePortal(TransportStage stage, int32_t code);

// What a plain HTTP request to our own host answered, read as: is someone else answering? Ours only
// ever redirects to itself over https. A page, a redirect anywhere else or a 511 is the network's.
// `location` may be null or empty, and is the raw header value.
bool portalAnswered(int status, const char* location, const char* host);

// dns_failed, tcp_failed, low_memory, tls_untrusted, tls_name, tls_clock, tls_anchor,
// tls_unsupported, tls_timeout, tls_failed, connection_lost, aborted, bad_url, captive_portal.
// A certificate outside its dates means tls_clock unless the clock is exact, when the
// certificate really is out of date and it means tls_untrusted. Null for no failure.
const char* failureCode(TransportStage stage, int32_t code, TimePrecision clock);

// Below the handshake's real floor (measured on RISC-V: 41.9 KB) wolfSSL often fails a signature
// check instead of an allocation, and says -155. So a verification failure on a heap that low is
// reported as what it is: telling the reader a certificate is untrusted would be false.
constexpr uint32_t kTlsFailureLowHeap = 45 * 1024;
int32_t heapAwareCode(TransportStage stage, int32_t code, uint32_t freeHeap);

// True for the failures a better clock could fix.
bool isClockFailure(TransportStage stage, int32_t code);

enum class LinkResult : uint8_t {
  Connected,
  NoSavedNetworks,  // Nothing in WifiCredentialStore.
  NotFound,         // No saved network is in range.
  AuthFailed,       // In range, and the saved password was refused.
  Timeout,          // Budget ran out.
  Cancelled,        // cancel.cancelled() returned true.
  RadioError,       // The Wi-Fi driver refused station mode or the scan.
};

// no_wifi, wifi_range, wifi_auth, wifi_timeout, cancelled, radio_error. Null for Connected.
const char* linkResultCode(LinkResult result);

// What a screen says about a link. Connected is here because a screen can get to this with the link
// up and something else having failed: the radio is then the one thing that cannot be blamed.
enum class LinkWords : uint8_t { NoSavedWifi, OutOfRange, WrongPassword, TookTooLong, RadioFailed, ServerUnreachable };
LinkWords linkWordsFor(LinkResult result);

// The two failures choosing another network can fix.
inline bool linkOffersNetworkPicker(const LinkResult result) {
  return result == LinkResult::NotFound || result == LinkResult::AuthFailed;
}

// wolfSSL allocates at most ~17 KB in one block with 16 KB records, and needs far more
// in total: below this a handshake would die in MP_MEM, or worse, half way.
constexpr uint32_t kHandshakeMinFreeHeap = 50 * 1024;
constexpr uint32_t kHandshakeMinLargestBlock = 20 * 1024;
inline bool heapAllowsHandshake(uint32_t freeBytes, uint32_t largestBlock) {
  return freeBytes >= kHandshakeMinFreeHeap && largestBlock >= kHandshakeMinLargestBlock;
}

}  // namespace net
}  // namespace borges
