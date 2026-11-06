#include "net/NetFailure.h"

namespace borges {
namespace net {

int32_t heapAwareCode(const TransportStage stage, const int32_t code, const uint32_t freeHeap) {
  if (stage != TransportStage::Tls || freeHeap >= kTlsFailureLowHeap) return code;
  const bool verification =
      code == wolfcode::kSigConfirm || code == wolfcode::kNoSigner || code == wolfcode::kVerifyCert;
  return verification ? kCodeLowMemory : code;
}

const char* linkResultCode(const LinkResult result) {
  switch (result) {
    case LinkResult::Connected:
      return nullptr;
    case LinkResult::NoSavedNetworks:
      return "no_wifi";
    case LinkResult::NotFound:
      return "wifi_range";
    case LinkResult::AuthFailed:
      return "wifi_auth";
    case LinkResult::Timeout:
      return "wifi_timeout";
    case LinkResult::Cancelled:
      return "cancelled";
    case LinkResult::RadioError:
      return "radio_error";
  }
  return "radio_error";
}

LinkWords linkWordsFor(const LinkResult result) {
  switch (result) {
    case LinkResult::NoSavedNetworks:
      return LinkWords::NoSavedWifi;
    case LinkResult::NotFound:
      return LinkWords::OutOfRange;
    case LinkResult::AuthFailed:
      return LinkWords::WrongPassword;
    case LinkResult::Timeout:
    case LinkResult::Cancelled:
      return LinkWords::TookTooLong;
    case LinkResult::Connected:
      return LinkWords::ServerUnreachable;
    case LinkResult::RadioError:
      break;
  }
  return LinkWords::RadioFailed;
}

const char* failureCode(const TransportStage stage, const int32_t code, const TimePrecision clock) {
  if (stage == TransportStage::None && code == 0) return nullptr;
  if (stage == TransportStage::Aborted) return "aborted";
  if (code == kCodeBadUrl) return "bad_url";
  if (code == kCodeLowMemory || code == wolfcode::kMemory || code == wolfcode::kMathMemory) return "low_memory";
  switch (stage) {
    case TransportStage::Dns:
      return "dns_failed";
    case TransportStage::Connect:
      return "tcp_failed";
    case TransportStage::Tls:
      switch (code) {
        case wolfcode::kNoSigner:
        case wolfcode::kSigConfirm:
        case wolfcode::kVerifyCert:
          return "tls_untrusted";
        case wolfcode::kNameMismatch:
          return "tls_name";
        case wolfcode::kBeforeDate:
        case wolfcode::kAfterDate:
          return clock == TimePrecision::Exact ? "tls_untrusted" : "tls_clock";
        case wolfcode::kHashType:
          return "tls_unsupported";
        case kCodeAnchorLoad:
          return "tls_anchor";
        case kCodeTimeout:
          return "tls_timeout";
        default:
          return "tls_failed";
      }
    case TransportStage::Send:
    case TransportStage::ReadHeaders:
    case TransportStage::ReadBody:
      return "connection_lost";
    default:
      return "connection_lost";
  }
}

bool isClockFailure(const TransportStage stage, const int32_t code) {
  return stage == TransportStage::Tls && (code == wolfcode::kBeforeDate || code == wolfcode::kAfterDate);
}

}  // namespace net
}  // namespace borges
