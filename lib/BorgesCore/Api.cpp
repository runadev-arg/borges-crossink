#include "Api.h"

#include <cstring>

#include "JsonStream.h"
#include "Store.h"

namespace borges {

Api::Api(IFs& fs, IHttp& http, IClock& clock, ICancel& cancel)
    : fs_(fs), http_(http), clock_(clock), cancel_(cancel), abort_(*this) {}

void Api::configure(const char* baseUrl, const char* bearerToken) {
  baseUrl_ = baseUrl ? baseUrl : "";
  token_ = bearerToken ? bearerToken : "";
}

void Api::beginSession(uint32_t budgetMs) {
  deadlineMs_ = clock_.monotonicMs() + budgetMs;
  reconnectUsed_ = false;
}

void Api::endSession() { http_.disconnect(); }

bool Api::cancelled() { return cancel_.cancelled(); }

bool Api::budgetExpired() { return clock_.monotonicMs() >= deadlineMs_; }

bool Api::canContinue() { return !cancelled() && !budgetExpired(); }

const char* Api::spoolPath() const { return kSpoolPath; }

void Api::perform(const char* method, const char* path, const uint8_t* body, size_t bodyLen, IHttpSink& sink,
                  const HttpHeader* extraHeaders, uint8_t extraCount, HttpResponse& out) {
  char url[256];
  const size_t baseLen = strlen(baseUrl_);
  const size_t pathLen = strlen(path);
  out = HttpResponse{};
  if (baseLen + pathLen + 1 > sizeof(url)) return;
  memcpy(url, baseUrl_, baseLen);
  memcpy(url + baseLen, path, pathLen + 1);

  char bearer[7 + 128 + 1];
  memcpy(bearer, "Bearer ", 7);
  copyText(bearer + 7, sizeof(bearer) - 7, token_);

  HttpHeader headers[6];
  uint8_t count = 0;
  headers[count++] = HttpHeader{"Accept", "application/json"};
  if (token_[0]) headers[count++] = HttpHeader{"Authorization", bearer};
  if (body) headers[count++] = HttpHeader{"Content-Type", "application/json"};
  for (uint8_t i = 0; i < extraCount && count < 6; ++i) headers[count++] = extraHeaders[i];

  HttpRequest request;
  request.method = method;
  request.url = url;
  request.headers = headers;
  request.headerCount = count;
  request.body = body;
  request.bodyLen = bodyLen;
  http_.perform(request, sink, abort_, out);
  memset(bearer, 0, sizeof(bearer));
}

ApiResult Api::callTo(const char* method, const char* path, IHttpSink& sink, const HttpHeader* extraHeaders,
                      uint8_t extraCount) {
  ApiResult result;
  if (cancelled()) {
    result.kind = ApiKind::Cancelled;
    return result;
  }
  if (budgetExpired()) {
    result.kind = ApiKind::Budget;
    return result;
  }
  perform(method, path, nullptr, 0, sink, extraHeaders, extraCount, response_);
  result.status = response_.status;
  result.stage = response_.stage;
  result.transportCode = response_.transportCode;
  result.hasRetryAfter = response_.hasRetryAfter;
  result.retryAfterSec = response_.retryAfterSec;
  copyText(result.requestId, sizeof(result.requestId), response_.requestId);
  copyText(result.etag, sizeof(result.etag), response_.etag);
  copyText(result.koreaderMd5, sizeof(result.koreaderMd5), response_.koreaderMd5);
  copyText(result.contentSha256, sizeof(result.contentSha256), response_.contentSha256);
  if (cancelled()) {
    result.kind = ApiKind::Cancelled;
  } else if (budgetExpired() && (response_.status <= 0 || !response_.complete)) {
    result.kind = ApiKind::Budget;
  } else if (response_.status <= 0 || !response_.complete) {
    result.kind = ApiKind::Transport;
  } else if (response_.status >= 200 && response_.status < 300) {
    result.kind = ApiKind::Ok;
  } else {
    result.kind = ApiKind::HttpError;
  }
  return result;
}

ApiResult Api::call(const char* method, const char* path, const uint8_t* body, size_t bodyLen,
                    const HttpHeader* extraHeaders, uint8_t extraCount) {
  ApiResult result;
  for (int attempt = 0;; ++attempt) {
    if (cancelled()) {
      result.kind = ApiKind::Cancelled;
      return result;
    }
    if (budgetExpired()) {
      result.kind = ApiKind::Budget;
      return result;
    }
    Spool spool(fs_, kSpoolPath);
    if (!spool.begin()) {
      result.kind = ApiKind::LocalIo;
      return result;
    }
    perform(method, path, body, bodyLen, spool, extraHeaders, extraCount, response_);
    spool.close();
    if (spool.ioFailed()) {
      result.kind = ApiKind::LocalIo;
      return result;
    }

    result = ApiResult{};
    result.status = response_.status;
    result.stage = response_.stage;
    result.transportCode = response_.transportCode;
    result.hasRetryAfter = response_.hasRetryAfter;
    result.retryAfterSec = response_.retryAfterSec;
    copyText(result.requestId, sizeof(result.requestId), response_.requestId);

    if (response_.status <= 0 || !response_.complete) {
      if (cancelled()) {
        result.kind = ApiKind::Cancelled;
        return result;
      }
      if (budgetExpired()) {
        result.kind = ApiKind::Budget;
        return result;
      }
      // A dropped socket earns one reconnect per session. Every request here is
      // safe to repeat: events carry immutable ids and pulls carry the same cursor.
      if (attempt == 0 && !reconnectUsed_) {
        reconnectUsed_ = true;
        http_.disconnect();
        continue;
      }
      result.kind = ApiKind::Transport;
      return result;
    }
    if (response_.status >= 200 && response_.status < 300) {
      result.kind = ApiKind::Ok;
      return result;
    }
    result.kind = ApiKind::HttpError;
    decodeError(result);
    return result;
  }
}

// {"error":{"code","message","retryable","details"},"request_id"}. Older routes use
// {"error":"text","code":"..."}. Anything else leaves the status as the only clue.
void Api::decodeError(ApiResult& result) {
  File f(fs_, kSpoolPath, IFs::Mode::Read);
  if (!f) return;
  FileByteSource source(f);
  JsonScanner scan(source);
  if (!scan.enterObject()) return;
  char key[16];
  while (scan.nextMember(key, sizeof(key))) {
    if (strcmp(key, "error") == 0) {
      if (scan.peek() == JsonType::String) {
        scan.readString(result.message, sizeof(result.message));
        continue;
      }
      if (!scan.enterObject()) {
        scan.skipValue();
        continue;
      }
      char inner[16];
      while (scan.nextMember(inner, sizeof(inner))) {
        if (strcmp(inner, "code") == 0) {
          bool truncated = false;
          scan.readString(result.code, sizeof(result.code), &truncated);
          if (truncated) result.code[0] = '\0';
        } else if (strcmp(inner, "message") == 0) {
          scan.readString(result.message, sizeof(result.message));
        } else if (strcmp(inner, "retryable") == 0) {
          scan.readBool(result.retryable);
        } else {
          scan.skipValue();
        }
      }
    } else if (strcmp(key, "code") == 0) {
      bool truncated = false;
      if (scan.peek() == JsonType::String) {
        scan.readString(result.code, sizeof(result.code), &truncated);
        if (truncated) result.code[0] = '\0';
      } else {
        scan.skipValue();
      }
    } else if (strcmp(key, "request_id") == 0 && !result.requestId[0]) {
      scan.readString(result.requestId, sizeof(result.requestId));
    } else {
      scan.skipValue();
    }
  }
}

}  // namespace borges
