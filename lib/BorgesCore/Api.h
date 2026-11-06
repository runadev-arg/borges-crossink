#pragma once

// One HTTP exchange the way every Borges endpoint wants it: Bearer token, JSON body,
// response spooled to SD, error body decoded, one reconnect per session on a dropped
// socket, and a deadline plus the cancel flag checked inside the stream.

#include <cstddef>
#include <cstdint>

#include "Ports.h"
#include "Spool.h"
#include "Types.h"

namespace borges {

enum class ApiKind : uint8_t {
  Ok,         // 2xx with a complete body in the spool.
  HttpError,  // Status >= 300, or 1xx; code/message decoded when the body had them.
  Transport,  // No complete response, after the one reconnect.
  Cancelled,
  Budget,
  LocalIo,  // The spool could not be written.
};

struct ApiResult {
  ApiKind kind = ApiKind::Transport;
  int status = 0;
  bool retryable = false;
  bool hasRetryAfter = false;
  uint32_t retryAfterSec = 0;
  TransportStage stage = TransportStage::None;
  int32_t transportCode = 0;
  char code[40] = {0};
  char message[64] = {0};
  char requestId[40] = {0};
  char etag[72] = {0};
  char koreaderMd5[33] = {0};
  char contentSha256[65] = {0};
};

class Api {
 public:
  Api(IFs& fs, IHttp& http, IClock& clock, ICancel& cancel);

  void configure(const char* baseUrl, const char* bearerToken);
  void beginSession(uint32_t budgetMs);
  void endSession();

  bool cancelled();
  bool budgetExpired();
  // True when another step may run.
  bool canContinue();

  // extraHeaders may be null. On Ok the body is in spoolPath().
  ApiResult call(const char* method, const char* path, const uint8_t* body, size_t bodyLen,
                 const HttpHeader* extraHeaders = nullptr, uint8_t extraCount = 0);
  // For what the server does only once and answers only once: a claim, a rotation. No reconnect,
  // because repeating it cannot bring back the answer it lost.
  ApiResult callOnce(const char* method, const char* path, const uint8_t* body, size_t bodyLen);
  // Same, but delivers the body to sink (for a file download) and does not decode errors.
  ApiResult callTo(const char* method, const char* path, IHttpSink& sink, const HttpHeader* extraHeaders,
                   uint8_t extraCount);

  const char* spoolPath() const;
  bool reconnectUsed() const { return reconnectUsed_; }

 private:
  class Abort : public ICancel {
   public:
    explicit Abort(Api& api) : api_(api) {}
    bool cancelled() override { return api_.cancelled() || api_.budgetExpired(); }

   private:
    Api& api_;
  };

  ApiResult request(const char* method, const char* path, const uint8_t* body, size_t bodyLen,
                    const HttpHeader* extraHeaders, uint8_t extraCount, bool mayRepeat);
  void perform(const char* method, const char* path, const uint8_t* body, size_t bodyLen, IHttpSink& sink,
               const HttpHeader* extraHeaders, uint8_t extraCount, HttpResponse& out);
  void decodeError(ApiResult& result);
  void copyTransport(ApiResult& result) const;

  IFs& fs_;
  IHttp& http_;
  IClock& clock_;
  ICancel& cancel_;
  Abort abort_;
  const char* baseUrl_ = "";
  const char* token_ = "";
  uint64_t deadlineMs_ = 0;
  bool reconnectUsed_ = false;
  HttpResponse response_;  // Kept off the stack: about 230 bytes.
};

}  // namespace borges
