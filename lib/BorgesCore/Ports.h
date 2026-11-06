#pragma once

// Every effect BorgesCore has on the world goes through one of these interfaces.
// Core code includes no Arduino or HAL header and allocates nothing behind them,
// so the device glue (HalStorage, SecureHttpClient) and the host fakes are
// interchangeable.

#include <cstddef>
#include <cstdint>

namespace borges {

// File system. Implementable on SdFat. Handles keep the core free of per-open
// allocations: the glue owns a small pool of FsFile objects (four is enough).
//
// What the core assumes:
//  - Data is durable only after flush() returned true.
//  - rename() over an existing file is NOT atomic, so the core never does it.
//    Atomicity comes from A/B slots (AtomicFile) and generation-named logs.
//  - A crash can leave any prefix of unflushed writes on disk.
class IFs {
 public:
  using Handle = int16_t;
  static constexpr Handle kInvalid = -1;
  enum class Mode : uint8_t {
    Read,        // Must exist. Position 0.
    WriteTrunc,  // Created or truncated. Position 0.
    Append,      // Created if missing. Every write goes to the end.
    Update,      // Must exist. Position 0. Writes replace bytes in place, on the same clusters.
  };

  virtual ~IFs() = default;
  virtual Handle open(const char* path, Mode mode) = 0;
  virtual int32_t read(Handle h, void* dst, size_t len) = 0;         // Bytes read, 0 at EOF, <0 on error.
  virtual int32_t write(Handle h, const void* src, size_t len) = 0;  // Bytes written, <0 on error.
  virtual bool seek(Handle h, uint32_t pos) = 0;
  virtual int64_t size(Handle h) = 0;  // <0 on error.
  virtual bool flush(Handle h) = 0;
  virtual void close(Handle h) = 0;
  virtual bool exists(const char* path) = 0;
  virtual bool remove(const char* path) = 0;                  // True if it no longer exists.
  virtual bool rename(const char* from, const char* to) = 0;  // Target must not exist.
  virtual bool mkdir(const char* path) = 0;                   // True if it exists afterwards.
};

// RAII for an IFs handle. A handle that goes out of scope is closed, on every path.
class File {
 public:
  File(IFs& fs, const char* path, IFs::Mode mode) : fs_(fs), h_(fs.open(path, mode)) {}
  ~File() { close(); }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  explicit operator bool() const { return h_ != IFs::kInvalid; }
  IFs::Handle handle() const { return h_; }
  int32_t read(void* dst, size_t len) { return fs_.read(h_, dst, len); }
  bool readExact(void* dst, size_t len) { return fs_.read(h_, dst, len) == static_cast<int32_t>(len); }
  bool writeAll(const void* src, size_t len) {
    return len == 0 || fs_.write(h_, src, len) == static_cast<int32_t>(len);
  }
  bool seek(uint32_t pos) { return fs_.seek(h_, pos); }
  int64_t size() { return fs_.size(h_); }
  bool flush() { return fs_.flush(h_); }
  void close() {
    if (h_ != IFs::kInvalid) fs_.close(h_);
    h_ = IFs::kInvalid;
  }

 private:
  IFs& fs_;
  IFs::Handle h_;
};

// Protocol values for time_precision. Ordered from worst to best, so comparing
// two values answers "which clock is more trustworthy".
enum class TimePrecision : uint8_t { Unknown = 0, Approximate = 1, Anchored = 2, Exact = 3 };

struct WallTime {
  int64_t epochSec = 0;
  TimePrecision precision = TimePrecision::Unknown;
};

class IClock {
 public:
  virtual ~IClock() = default;
  virtual uint64_t monotonicMs() = 0;                                   // Since boot. Never goes backwards.
  virtual WallTime wallNow() = 0;                                       // System clock, with its honest label.
  virtual void setWall(int64_t epochSec, TimePrecision precision) = 0;  // settimeofday + remember the label.
};

class IRandom {
 public:
  virtual ~IRandom() = default;
  virtual void fill(uint8_t* dst, size_t len) = 0;
};

// Polled between all steps and inside streams.
class ICancel {
 public:
  virtual ~ICancel() = default;
  virtual bool cancelled() = 0;
};

// Keeps a secret off the card: the key lives on the chip, so a card read elsewhere yields
// nothing. Authenticated: open() fails on another chip's data or on one flipped bit.
class ISecretBox {
 public:
  static constexpr size_t kOverhead = 28;  // nonce(12) + tag(16)
  virtual ~ISecretBox() = default;
  virtual bool seal(const uint8_t* plain, size_t len, uint8_t* out) = 0;   // out holds len + kOverhead.
  virtual bool open(const uint8_t* sealed, size_t len, uint8_t* out) = 0;  // out holds len - kOverhead.
};

class ISha256 {
 public:
  virtual ~ISha256() = default;
  virtual void begin() = 0;
  virtual void update(const void* data, size_t len) = 0;
  virtual void finish(uint8_t digest[32]) = 0;
};

struct HttpHeader {
  const char* name;
  const char* value;
};

struct HttpRequest {
  const char* method = "GET";
  const char* url = "";
  const HttpHeader* headers = nullptr;
  uint8_t headerCount = 0;
  const uint8_t* body = nullptr;
  size_t bodyLen = 0;
};

enum class TransportStage : uint8_t { None, Dns, Connect, Tls, Send, ReadHeaders, ReadBody, Aborted };

struct HttpResponse {
  int status = 0;         // HTTP status; <=0 when no status line was read.
  bool complete = false;  // The whole body reached the sink.
  bool hasRetryAfter = false;
  TransportStage stage = TransportStage::None;
  int32_t transportCode = 0;
  uint32_t retryAfterSec = 0;
  char requestId[40] = {0};      // X-Request-Id
  char etag[72] = {0};           // ETag, verbatim, including the quotes.
  char contentSha256[65] = {0};  // X-Content-SHA256
  char koreaderMd5[33] = {0};    // X-KOReader-Partial-MD5
};

class IHttpSink {
 public:
  virtual ~IHttpSink() = default;
  // Called once, with the status, before the first body byte.
  virtual void onStatus(int status) { (void)status; }
  // Returning false aborts the transfer.
  virtual bool onBody(const uint8_t* data, size_t len) = 0;
};

// One keep-alive connection per session. On SecureHttpClient, perform() maps to
// begin() + addHeader() + sendRequest(method, body, len, onData, shouldAbort),
// and disconnect() to end().
class IHttp {
 public:
  virtual ~IHttp() = default;
  virtual void perform(const HttpRequest& request, IHttpSink& sink, ICancel& abort, HttpResponse& out) = 0;
  virtual void disconnect() = 0;
};

}  // namespace borges
