// borges-cli: the real lib/BorgesCore engine behind a line-oriented command interface,
// with host implementations of its ports. Everything the engine decides is the engine's;
// this file only moves bytes and prints what happened.
//
//   borges-cli --sd DIR --mac a1b2c3d4e5f6 --server http://127.0.0.1:1234 [options] [--script]
//
// With --script, commands come one per line on stdin, tab separated, and every command
// answers one JSON line on stdout. Otherwise the remaining arguments are one command.
// A command is a name followed by key=value tokens.
//
// One process is one boot of the reader: RAM state (a pending prompt, the clock label)
// lasts as long as the process, and everything durable is on the SD directory.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <io.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <vector>

#include "Catalog.h"
#include "Codec.h"
#include "Events.h"
#include "JsonStream.h"
#include "Outbox.h"
#include "PairingFlow.h"
#include "Ports.h"
#include "Store.h"
#include "SyncEngine.h"
#include "Types.h"

// ---------------------------------------------------------------------------------
// Heap accounting. Only allocations made while core code runs are counted, so the
// numbers are the engine's and not this file's strings. Sizes are what was requested:
// a real allocator adds its own headers on top.
// ---------------------------------------------------------------------------------

namespace heap {
bool inCore = false;
size_t live = 0;
size_t peak = 0;
size_t allocations = 0;
size_t largest = 0;

struct Header {
  size_t size;
  size_t counted;
};
static_assert(sizeof(Header) == 16);

void* allocate(size_t size) {
  auto* h = static_cast<Header*>(std::malloc(size + sizeof(Header)));
  if (!h) return nullptr;
  h->size = size;
  h->counted = inCore ? 1 : 0;
  if (inCore) {
    live += size;
    ++allocations;
    if (size > largest) largest = size;
    if (live > peak) peak = live;
  }
  return h + 1;
}

void release(void* p) {
  if (!p) return;
  Header* h = static_cast<Header*>(p) - 1;
  if (h->counted) live -= h->size;
  std::free(h);
}

class Scope {
 public:
  Scope() { inCore = true; }
  ~Scope() { inCore = false; }
};
}  // namespace heap

void* operator new(size_t size) {
  void* p = heap::allocate(size);
  if (!p) std::abort();
  return p;
}
void* operator new[](size_t size) {
  void* p = heap::allocate(size);
  if (!p) std::abort();
  return p;
}
void* operator new(size_t size, const std::nothrow_t&) noexcept { return heap::allocate(size); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { return heap::allocate(size); }
void operator delete(void* p) noexcept { heap::release(p); }
void operator delete[](void* p) noexcept { heap::release(p); }
void operator delete(void* p, size_t) noexcept { heap::release(p); }
void operator delete[](void* p, size_t) noexcept { heap::release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { heap::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { heap::release(p); }

namespace {

using namespace borges;

// ---------------------------------------------------------------------------------
// IFs on the real file system. Strict on purpose: opening a file in a directory that
// does not exist fails, as it does on SdFat.
// ---------------------------------------------------------------------------------

class HostFs : public IFs {
 public:
  explicit HostFs(std::string root) : root_(std::move(root)) {
    for (auto& s : slots_) s.handle = INVALID_HANDLE_VALUE;
  }

  Handle open(const char* path, Mode mode) override {
    wchar_t wide[1024];
    if (!toWide(path, wide)) return kInvalid;
    int slot = -1;
    for (int i = 0; i < kSlots; ++i) {
      if (slots_[i].handle == INVALID_HANDLE_VALUE) {
        slot = i;
        break;
      }
    }
    if (slot < 0) {
      ++exhausted_;
      return kInvalid;
    }
    const DWORD access = mode == Mode::Read ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE;
    const DWORD disposition = mode == Mode::Read ? OPEN_EXISTING : mode == Mode::WriteTrunc ? CREATE_ALWAYS : OPEN_ALWAYS;
    HANDLE h = CreateFileW(wide, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disposition,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return kInvalid;
    slots_[slot].handle = h;
    slots_[slot].append = mode == Mode::Append;
    if (++open_ > peakOpen_) peakOpen_ = open_;
    return static_cast<Handle>(slot);
  }

  int32_t read(Handle h, void* dst, size_t len) override {
    if (!valid(h)) return -1;
    DWORD got = 0;
    if (!ReadFile(slots_[h].handle, dst, static_cast<DWORD>(len), &got, nullptr)) return -1;
    return static_cast<int32_t>(got);
  }

  int32_t write(Handle h, const void* src, size_t len) override {
    if (!valid(h)) return -1;
    if (slots_[h].append) {
      LARGE_INTEGER zero{};
      if (!SetFilePointerEx(slots_[h].handle, zero, nullptr, FILE_END)) return -1;
    }
    DWORD put = 0;
    if (!WriteFile(slots_[h].handle, src, static_cast<DWORD>(len), &put, nullptr)) return -1;
    return static_cast<int32_t>(put);
  }

  bool seek(Handle h, uint32_t pos) override {
    if (!valid(h)) return false;
    LARGE_INTEGER to{};
    to.QuadPart = pos;
    return SetFilePointerEx(slots_[h].handle, to, nullptr, FILE_BEGIN) != 0;
  }

  int64_t size(Handle h) override {
    if (!valid(h)) return -1;
    LARGE_INTEGER out{};
    if (!GetFileSizeEx(slots_[h].handle, &out)) return -1;
    return out.QuadPart;
  }

  // Written bytes are already with the kernel, and this harness kills processes rather
  // than the machine. Crash ordering is CrashMatrixTest's job, not this one's.
  bool flush(Handle h) override { return valid(h); }

  void close(Handle h) override {
    if (!valid(h)) return;
    CloseHandle(slots_[h].handle);
    slots_[h].handle = INVALID_HANDLE_VALUE;
    --open_;
  }

  bool exists(const char* path) override {
    wchar_t wide[1024];
    return toWide(path, wide) && GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES;
  }

  bool remove(const char* path) override {
    wchar_t wide[1024];
    if (!toWide(path, wide)) return false;
    if (GetFileAttributesW(wide) == INVALID_FILE_ATTRIBUTES) return true;
    return DeleteFileW(wide) != 0;
  }

  bool rename(const char* from, const char* to) override {
    wchar_t a[1024], b[1024];
    if (!toWide(from, a) || !toWide(to, b)) return false;
    return MoveFileExW(a, b, 0) != 0;
  }

  bool mkdir(const char* path) override {
    wchar_t wide[1024];
    if (!toWide(path, wide)) return false;
    if (CreateDirectoryW(wide, nullptr)) return true;
    const DWORD attrs = GetFileAttributesW(wide);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
  }

  int peakOpen() const { return peakOpen_; }
  int exhausted() const { return exhausted_; }
  int stillOpen() const { return open_; }

 private:
  static constexpr int kSlots = 8;
  struct Slot {
    HANDLE handle;
    bool append;
  };
  bool valid(Handle h) const { return h >= 0 && h < kSlots && slots_[h].handle != INVALID_HANDLE_VALUE; }
  bool toWide(const char* path, wchar_t (&out)[1024]) const {
    char full[1024];
    const size_t rootLen = root_.size();
    const size_t pathLen = strlen(path);
    if (rootLen + pathLen + 1 > sizeof(full)) return false;
    memcpy(full, root_.data(), rootLen);
    memcpy(full + rootLen, path, pathLen + 1);
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, full, -1, out, 1024) > 0;
  }
  std::string root_;
  Slot slots_[kSlots];
  int open_ = 0;
  int peakOpen_ = 0;
  int exhausted_ = 0;
};

// ---------------------------------------------------------------------------------
// Clock, random, cancel.
// ---------------------------------------------------------------------------------

class HostClock : public IClock {
 public:
  uint64_t monotonicMs() override { return GetTickCount64() - start_ + monoOffset; }
  WallTime wallNow() override { return WallTime{static_cast<int64_t>(time(nullptr)) + wallOffset, precision}; }
  void setWall(int64_t epochSec, TimePrecision p) override {
    wallOffset = epochSec - static_cast<int64_t>(time(nullptr));
    precision = p;
    ++setCalls;
  }
  uint64_t monoOffset = 0;
  int64_t wallOffset = 0;
  TimePrecision precision = TimePrecision::Exact;
  int setCalls = 0;

 private:
  uint64_t start_ = GetTickCount64();
};

class HostRandom : public IRandom {
 public:
  void fill(uint8_t* dst, size_t len) override {
    if (BCryptGenRandom(nullptr, dst, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) std::abort();
  }
};

class NeverCancel : public ICancel {
 public:
  bool cancelled() override { return false; }
};

// ---------------------------------------------------------------------------------
// IHttp over a blocking Winsock socket: one keep-alive connection, no retries of its
// own (a dropped socket is the core's to handle), abort polled while waiting.
// ---------------------------------------------------------------------------------

class HostHttp : public IHttp {
 public:
  explicit HostHttp(HostClock& clock) : clock_(clock) {}
  ~HostHttp() override { disconnect(); }

  uint64_t monoStepMs = 0;  // Added to the clock per request, to spend a budget without waiting.
  uint32_t requests = 0;
  uint32_t connections = 0;

  void disconnect() override {
    if (sock_ != INVALID_SOCKET) closesocket(sock_);
    sock_ = INVALID_SOCKET;
    leftover_ = 0;
  }

  void perform(const HttpRequest& req, IHttpSink& sink, ICancel& abort, HttpResponse& out) override {
    out = HttpResponse{};
    ++requests;
    clock_.monoOffset += monoStepMs;

    // http://host:port/path
    if (strncmp(req.url, "http://", 7) != 0) {
      fail(out, TransportStage::Connect, -1);
      return;
    }
    const char* authority = req.url + 7;
    const char* slash = strchr(authority, '/');
    if (!slash) slash = authority + strlen(authority);
    const std::string hostPort(authority, slash);  // heap::inCore: counted, about one small string a request.
    if (sock_ != INVALID_SOCKET && hostPort != connectedTo_) disconnect();
    if (sock_ == INVALID_SOCKET && !connectTo(hostPort, out)) return;

    char head[1024];
    int n = snprintf(head, sizeof(head), "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n", req.method,
                     *slash ? slash : "/", hostPort.c_str());
    for (uint8_t i = 0; i < req.headerCount; ++i) {
      n += snprintf(head + n, sizeof(head) - n, "%s: %s\r\n", req.headers[i].name, req.headers[i].value);
    }
    if (req.body) n += snprintf(head + n, sizeof(head) - n, "Content-Length: %u\r\n", static_cast<unsigned>(req.bodyLen));
    n += snprintf(head + n, sizeof(head) - n, "\r\n");
    if (n >= static_cast<int>(sizeof(head))) {
      fail(out, TransportStage::Send, -2);
      return;
    }
    if (!sendAll(head, n) || (req.body && !sendAll(reinterpret_cast<const char*>(req.body), req.bodyLen))) {
      fail(out, TransportStage::Send, WSAGetLastError());
      disconnect();
      return;
    }

    readResponse(sink, abort, out);
  }

 private:
  static void fail(HttpResponse& out, TransportStage stage, int32_t code) {
    out.stage = stage;
    out.transportCode = code;
  }

  bool connectTo(const std::string& hostPort, HttpResponse& out) {
    const size_t colon = hostPort.rfind(':');
    const std::string host = hostPort.substr(0, colon);
    // This binary only ever talks to the harness: no DNS, no TLS, no other host.
    if (host != "127.0.0.1") {
      fail(out, TransportStage::Dns, -9);
      return false;
    }
    const int port = colon == std::string::npos ? 80 : atoi(hostPort.c_str() + colon + 1);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
      fail(out, TransportStage::Dns, -1);
      return false;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
      fail(out, TransportStage::Connect, WSAGetLastError());
      return false;
    }
    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      fail(out, TransportStage::Connect, WSAGetLastError());
      closesocket(s);
      return false;
    }
    sock_ = s;
    connectedTo_ = hostPort;
    leftover_ = 0;
    ++connections;
    return true;
  }

  bool sendAll(const char* data, size_t len) {
    while (len) {
      const int sent = send(sock_, data, static_cast<int>(len), 0);
      if (sent <= 0) return false;
      data += sent;
      len -= static_cast<size_t>(sent);
    }
    return true;
  }

  // >0 bytes, 0 orderly close, -1 error, -2 aborted.
  int recvSome(char* dst, int cap, ICancel& abort) {
    for (;;) {
      if (abort.cancelled()) return -2;
      fd_set set;
      FD_ZERO(&set);
      FD_SET(sock_, &set);
      timeval tv{0, 50000};
      const int ready = select(0, &set, nullptr, nullptr, &tv);
      if (ready < 0) return -1;
      if (ready == 0) continue;
      const int got = recv(sock_, dst, cap, 0);
      if (got < 0) return -1;
      return got;
    }
  }

  void readResponse(IHttpSink& sink, ICancel& abort, HttpResponse& out) {
    size_t have = 0;
    size_t headerEnd = 0;
    for (;;) {
      for (size_t i = 3; i < have; ++i) {
        if (buffer_[i - 3] == '\r' && buffer_[i - 2] == '\n' && buffer_[i - 1] == '\r' && buffer_[i] == '\n') {
          headerEnd = i + 1;
          break;
        }
      }
      if (headerEnd) break;
      if (have == sizeof(buffer_)) {
        fail(out, TransportStage::ReadHeaders, -3);
        disconnect();
        return;
      }
      const int got = recvSome(buffer_ + have, static_cast<int>(sizeof(buffer_) - have), abort);
      if (got == -2) {
        fail(out, TransportStage::Aborted, 0);
        disconnect();
        return;
      }
      if (got <= 0) {
        fail(out, TransportStage::ReadHeaders, got == 0 ? -7 : WSAGetLastError());
        disconnect();
        return;
      }
      have += static_cast<size_t>(got);
    }

    int64_t contentLength = -1;
    bool chunked = false;
    bool closeAfter = false;
    {
      char* line = buffer_;
      char* end = buffer_ + headerEnd;
      bool first = true;
      while (line < end) {
        char* eol = line;
        while (eol + 1 < end && !(eol[0] == '\r' && eol[1] == '\n')) ++eol;
        *eol = '\0';
        if (first) {
          const char* space = strchr(line, ' ');
          out.status = space ? atoi(space + 1) : 0;
          first = false;
        } else if (char* colon = strchr(line, ':')) {
          *colon = '\0';
          const char* value = colon + 1;
          while (*value == ' ') ++value;
          if (_stricmp(line, "content-length") == 0) contentLength = _atoi64(value);
          if (_stricmp(line, "transfer-encoding") == 0 && _stricmp(value, "chunked") == 0) chunked = true;
          if (_stricmp(line, "connection") == 0 && _stricmp(value, "close") == 0) closeAfter = true;
          if (_stricmp(line, "x-request-id") == 0) copyText(out.requestId, sizeof(out.requestId), value);
          if (_stricmp(line, "etag") == 0) copyText(out.etag, sizeof(out.etag), value);
          if (_stricmp(line, "x-content-sha256") == 0) copyText(out.contentSha256, sizeof(out.contentSha256), value);
          if (_stricmp(line, "x-koreader-partial-md5") == 0) copyText(out.koreaderMd5, sizeof(out.koreaderMd5), value);
          if (_stricmp(line, "retry-after") == 0) {
            out.hasRetryAfter = true;
            out.retryAfterSec = static_cast<uint32_t>(strtoul(value, nullptr, 10));
          }
        }
        line = eol + 2;
      }
    }
    if (out.status <= 0) {
      fail(out, TransportStage::ReadHeaders, -4);
      disconnect();
      return;
    }

    sink.onStatus(out.status);
    pending_ = buffer_ + headerEnd;
    pendingLen_ = have - headerEnd;

    const bool ok = chunked ? readChunked(sink, abort, out) : readPlain(sink, abort, out, contentLength);
    if (!ok) {
      disconnect();
      return;
    }
    out.complete = true;
    if (closeAfter || (contentLength < 0 && !chunked)) disconnect();
  }

  int nextBytes(char* dst, int cap, ICancel& abort) {
    if (pendingLen_) {
      const size_t n = pendingLen_ < static_cast<size_t>(cap) ? pendingLen_ : static_cast<size_t>(cap);
      memmove(dst, pending_, n);
      pending_ += n;
      pendingLen_ -= n;
      return static_cast<int>(n);
    }
    return recvSome(dst, cap, abort);
  }

  bool readPlain(IHttpSink& sink, ICancel& abort, HttpResponse& out, int64_t contentLength) {
    char chunk[4096];
    int64_t left = contentLength;
    while (left != 0) {
      const int want = left < 0 || left > static_cast<int64_t>(sizeof(chunk)) ? static_cast<int>(sizeof(chunk)) : static_cast<int>(left);
      const int got = nextBytes(chunk, want, abort);
      if (got == -2) {
        fail(out, TransportStage::Aborted, 0);
        return false;
      }
      if (got < 0 || (got == 0 && left > 0)) {
        fail(out, TransportStage::ReadBody, got == 0 ? -7 : WSAGetLastError());
        return false;
      }
      if (got == 0) return true;  // Close-delimited body.
      if (!sink.onBody(reinterpret_cast<uint8_t*>(chunk), static_cast<size_t>(got))) {
        fail(out, TransportStage::ReadBody, -8);
        return false;
      }
      if (left > 0) left -= got;
    }
    return true;
  }

  // One byte at a time for the size lines: they are short and this is a test host.
  bool readLine(char* line, size_t cap, ICancel& abort, HttpResponse& out) {
    size_t n = 0;
    for (;;) {
      char c;
      const int got = nextBytes(&c, 1, abort);
      if (got <= 0) {
        fail(out, got == -2 ? TransportStage::Aborted : TransportStage::ReadBody, got == 0 ? -7 : got);
        return false;
      }
      if (c == '\n') break;
      if (c != '\r' && n + 1 < cap) line[n++] = c;
    }
    line[n] = '\0';
    return true;
  }

  bool readChunked(IHttpSink& sink, ICancel& abort, HttpResponse& out) {
    char line[32];
    char chunk[4096];
    for (;;) {
      if (!readLine(line, sizeof(line), abort, out)) return false;
      size_t left = strtoul(line, nullptr, 16);
      if (left == 0) {
        for (;;) {
          if (!readLine(line, sizeof(line), abort, out)) return false;
          if (!line[0]) return true;
        }
      }
      while (left) {
        const int want = left > sizeof(chunk) ? static_cast<int>(sizeof(chunk)) : static_cast<int>(left);
        const int got = nextBytes(chunk, want, abort);
        if (got <= 0) {
          fail(out, got == -2 ? TransportStage::Aborted : TransportStage::ReadBody, got == 0 ? -7 : got);
          return false;
        }
        if (!sink.onBody(reinterpret_cast<uint8_t*>(chunk), static_cast<size_t>(got))) {
          fail(out, TransportStage::ReadBody, -8);
          return false;
        }
        left -= static_cast<size_t>(got);
      }
      if (!readLine(line, sizeof(line), abort, out)) return false;
    }
  }

  HostClock& clock_;
  SOCKET sock_ = INVALID_SOCKET;
  std::string connectedTo_;
  char buffer_[16384];
  char* pending_ = nullptr;
  size_t pendingLen_ = 0;
  size_t leftover_ = 0;
};

// ---------------------------------------------------------------------------------
// JSON output.
// ---------------------------------------------------------------------------------

std::string esc(const char* text) {
  std::string out = "\"";
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text ? text : ""); *p; ++p) {
    if (*p == '"' || *p == '\\') {
      out += '\\';
      out += static_cast<char>(*p);
    } else if (*p < 0x20) {
      char u[8];
      snprintf(u, sizeof(u), "\\u%04x", *p);
      out += u;
    } else {
      out += static_cast<char>(*p);
    }
  }
  out += '"';
  return out;
}
std::string esc(const std::string& text) { return esc(text.c_str()); }
const char* boolText(bool v) { return v ? "true" : "false"; }

const char* outcomeName(SessionOutcome o) {
  switch (o) {
    case SessionOutcome::Ok: return "ok";
    case SessionOutcome::AwaitingChoice: return "awaiting_choice";
    case SessionOutcome::NotPaired: return "not_paired";
    case SessionOutcome::Cancelled: return "cancelled";
    case SessionOutcome::BudgetExhausted: return "budget_exhausted";
    case SessionOutcome::NeedsRelink: return "needs_relink";
    case SessionOutcome::RateLimited: return "rate_limited";
    case SessionOutcome::Transport: return "transport";
    case SessionOutcome::ServerError: return "server_error";
    case SessionOutcome::ProtocolError: return "protocol_error";
    case SessionOutcome::LocalError: return "local_error";
    case SessionOutcome::StaleChoice: return "stale_choice";
  }
  return "?";
}
const char* verdictName(resume::Verdict v) {
  switch (v) {
    case resume::Verdict::NoEvent: return "no_event";
    case resume::Verdict::Own: return "own";
    case resume::Verdict::AlreadyChosen: return "already_chosen";
    case resume::Verdict::Invalid: return "invalid";
    case resume::Verdict::Ask: return "ask";
  }
  return "?";
}
const char* gateName(Gate g) {
  switch (g) {
    case Gate::Go: return "go";
    case Gate::NotPaired: return "not_paired";
    case Gate::NeedsRelink: return "needs_relink";
    case Gate::BackedOff: return "backed_off";
    case Gate::NothingToDo: return "nothing_to_do";
  }
  return "?";
}
const char* pairingName(PairingState s) {
  switch (s) {
    case PairingState::Idle: return "idle";
    case PairingState::WaitingForApproval: return "waiting";
    case PairingState::Done: return "done";
    case PairingState::Rejected: return "rejected";
    case PairingState::Expired: return "expired";
    case PairingState::ClaimLost: return "claim_lost";
    case PairingState::Failed: return "failed";
  }
  return "?";
}
const char* catalogName(CatalogResult r) {
  switch (r) {
    case CatalogResult::Ok: return "ok";
    case CatalogResult::Offline: return "offline";
    case CatalogResult::Cancelled: return "cancelled";
    case CatalogResult::NeedsRelink: return "needs_relink";
    case CatalogResult::ServerError: return "server_error";
    case CatalogResult::Malformed: return "malformed";
    case CatalogResult::LocalError: return "local_error";
  }
  return "?";
}
const char* downloadName(DownloadResult r) {
  switch (r) {
    case DownloadResult::Done: return "done";
    case DownloadResult::AlreadyPresent: return "already_present";
    case DownloadResult::Interrupted: return "interrupted";
    case DownloadResult::Cancelled: return "cancelled";
    case DownloadResult::NeedsRelink: return "needs_relink";
    case DownloadResult::ServerError: return "server_error";
    case DownloadResult::IntegrityFailed: return "integrity_failed";
    case DownloadResult::LocalError: return "local_error";
  }
  return "?";
}

std::string reportJson(const SessionReport& r) {
  std::ostringstream o;
  o << "{\"outcome\":" << esc(outcomeName(r.outcome)) << ",\"focus_checked\":" << boolText(r.focusChecked)
    << ",\"focus_invalid\":" << boolText(r.focusInvalid) << ",\"pull_failed\":" << boolText(r.pullFailed)
    << ",\"drain_blocked_by_pull\":" << boolText(r.drainBlockedByPull) << ",\"cursor_recovered\":" << boolText(r.cursorRecovered)
    << ",\"sequence_repaired\":" << boolText(r.sequenceRepaired) << ",\"pages_pulled\":" << static_cast<int>(r.pagesPulled)
    << ",\"events_applied\":" << r.eventsApplied << ",\"events_skipped\":" << r.eventsSkipped << ",\"events_sent\":" << r.eventsSent
    << ",\"events_acked\":" << r.eventsAcked << ",\"events_rejected\":" << r.eventsRejected
    << ",\"events_quarantined\":" << r.eventsQuarantined << ",\"depth\":" << r.depth << ",\"held\":" << r.held << "}";
  return o.str();
}

std::string decisionJson(const ResumeDecision& d) {
  std::ostringstream o;
  o << "{\"ask\":" << boolText(d.ask) << ",\"verdict\":" << esc(verdictName(d.verdict)) << ",\"book\":" << esc(d.book.hex)
    << ",\"has_remote_pct\":" << boolText(d.hasRemotePct) << ",\"remote_pct\":" << (d.remotePct / 1000000.0)
    << ",\"xpointer_usable\":" << boolText(d.xpointerUsable) << ",\"remote_xpointer\":" << esc(d.remoteXpointer)
    << ",\"remote_device\":" << esc(d.remoteDeviceName) << ",\"occurred_at\":" << esc(d.occurredAt)
    << ",\"show_date\":" << boolText(d.showDate) << ",\"warnings\":" << static_cast<int>(d.warnings)
    << ",\"fingerprint\":\"" << d.fingerprint << "\"}";
  return o.str();
}

std::string diagJson(const Diagnostics& d) {
  std::ostringstream o;
  o << "{\"stage\":" << static_cast<int>(d.stage) << ",\"http_status\":" << d.httpStatus << ",\"transport_stage\":"
    << static_cast<int>(d.transportStage) << ",\"transport_code\":" << d.transportCode << ",\"code\":" << esc(d.code)
    << ",\"detail\":" << esc(d.detail) << ",\"request_id\":" << esc(d.requestId) << "}";
  return o.str();
}

std::string positionJson(const Position& p) {
  std::ostringstream o;
  o << "{\"has_pct\":" << boolText(p.hasPct) << ",\"pct\":" << (p.pctMicro / 1000000.0) << ",\"page\":" << p.page
    << ",\"total\":" << p.totalPages << ",\"xpointer\":" << esc(p.xpointer) << "}";
  return o.str();
}

using Args = std::map<std::string, std::string>;

std::string arg(const Args& a, const char* key, const char* fallback = "") {
  auto it = a.find(key);
  return it == a.end() ? fallback : it->second;
}
bool hasArg(const Args& a, const char* key) { return a.count(key) != 0; }
int64_t argInt(const Args& a, const char* key, int64_t fallback) {
  auto it = a.find(key);
  return it == a.end() ? fallback : _atoi64(it->second.c_str());
}

TimePrecision parsePrecision(const std::string& s) {
  if (s == "exact") return TimePrecision::Exact;
  if (s == "anchored") return TimePrecision::Anchored;
  if (s == "approximate") return TimePrecision::Approximate;
  return TimePrecision::Unknown;
}

// ---------------------------------------------------------------------------------
// The book's clippings, from a TSV prepared by the test. One row per clip:
// spine offset hash bookmark text note chapter color xpointer page pctMicro timeMs
// \t, \n and \\ are escaped. A first line UNREADABLE makes count() fail.
// ---------------------------------------------------------------------------------

std::string unescape(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      ++i;
      out += s[i] == 't' ? '\t' : s[i] == 'n' ? '\n' : s[i];
    } else {
      out += s[i];
    }
  }
  return out;
}

class TsvClips : public IClipSource {
 public:
  explicit TsvClips(const std::string& hostPath) {
    std::ifstream in(hostPath, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line == "UNREADABLE") {
        unreadable_ = true;
        continue;
      }
      if (line.empty()) continue;
      std::vector<std::string> f;
      size_t start = 0;
      for (;;) {
        const size_t tab = line.find('\t', start);
        f.push_back(unescape(line.substr(start, tab == std::string::npos ? tab : tab - start)));
        if (tab == std::string::npos) break;
        start = tab + 1;
      }
      f.resize(12);
      rows_.push_back(std::move(f));
    }
  }
  bool count(size_t& out) override {
    if (unreadable_) return false;
    out = rows_.size();
    return true;
  }
  bool key(size_t i, ClipKey& out) override {
    if (i >= rows_.size()) return false;
    const auto& f = rows_[i];
    out.spine = static_cast<uint32_t>(strtoul(f[0].c_str(), nullptr, 10));
    out.textOffset = static_cast<uint32_t>(strtoul(f[1].c_str(), nullptr, 10));
    out.textHash = strtoull(f[2].c_str(), nullptr, 10);
    out.bookmark = f[3] == "1";
    return true;
  }
  bool detail(size_t i, ClipDetail& out) override {
    if (i >= rows_.size()) return false;
    const auto& f = rows_[i];
    out = ClipDetail{};
    out.text = f[4].c_str();
    out.note = f[5].c_str();
    out.chapter = f[6].c_str();
    out.color = f[7].empty() ? nullptr : f[7].c_str();
    out.xpointer = f[8].empty() ? nullptr : f[8].c_str();
    out.page = f[9].empty() ? -1 : atoi(f[9].c_str());
    if (!f[10].empty() && f[10] != "-1") {
      out.hasPct = true;
      out.pctMicro = static_cast<uint32_t>(strtoul(f[10].c_str(), nullptr, 10));
    }
    out.timeMs = f[11].empty() ? 0 : _atoi64(f[11].c_str());
    return true;
  }

 private:
  bool unreadable_ = false;
  std::vector<std::vector<std::string>> rows_;
};

// ---------------------------------------------------------------------------------
// A booted device.
// ---------------------------------------------------------------------------------

struct Options {
  std::string sd;
  std::string macHex = "a1b2c3d4e5f6";
  std::string server;
  std::string deviceName = "Xteink X4";
  std::string firmware = "1.6.1-contract";
  std::string client = "crossink-borges/1.6.1-contract";
  std::string externalId;
  std::string precision = "exact";
  int64_t wallOffset = 0;
  int64_t monoStepMs = 0;
  int64_t openBudgetMs = -1;
  int64_t sleepBudgetMs = -1;
  int64_t manualBudgetMs = -1;
  int64_t maxBodyBytes = -1;
  int64_t maxPullPages = -1;
  int64_t pullLimit = -1;
  int64_t eventParseBudget = -1;
  bool script = false;
};

class Device {
 public:
  explicit Device(const Options& opt)
      : opt_(opt),
        // Store copies the key when constructed, so the MAC has to be decoded before it.
        fs_((hexDecode(opt.macHex.c_str(), mac_, 6), opt.sd)),
        http_(clock_),
        store_(fs_, mac_, 6),
        outbox_(fs_, random_, kOutboxDir) {
    clock_.precision = parsePrecision(opt.precision);
    clock_.wallOffset = opt.wallOffset;
    http_.monoStepMs = static_cast<uint64_t>(opt.monoStepMs);
    config_.firmwareVersion = opt_.firmware.c_str();
    config_.clientVersion = opt_.client.c_str();
    if (opt.openBudgetMs >= 0) config_.openBudgetMs = static_cast<uint32_t>(opt.openBudgetMs);
    if (opt.sleepBudgetMs >= 0) config_.sleepBudgetMs = static_cast<uint32_t>(opt.sleepBudgetMs);
    if (opt.manualBudgetMs >= 0) config_.manualBudgetMs = static_cast<uint32_t>(opt.manualBudgetMs);
    if (opt.maxBodyBytes >= 0) config_.maxBodyBytes = static_cast<uint32_t>(opt.maxBodyBytes);
    if (opt.maxPullPages >= 0) config_.maxPullPages = static_cast<uint8_t>(opt.maxPullPages);
    if (opt.pullLimit >= 0) config_.pullLimit = static_cast<uint8_t>(opt.pullLimit);
    if (opt.eventParseBudget >= 0) config_.eventParseBudget = static_cast<uint32_t>(opt.eventParseBudget);
    if (opt.externalId.empty()) {
      char id[20];
      makeExternalId(mac_, id);
      externalId_ = id;
    } else {
      externalId_ = opt.externalId;
    }
  }

  bool boot() {
    heap::Scope scope;
    if (!store_.begin()) return false;
    // In RAM, as the simulator does: nothing on the card can move a real device.
    if (!opt_.server.empty()) copyText(store_.creds().baseUrl, sizeof(store_.creds().baseUrl), opt_.server.c_str());
    engine_ = std::unique_ptr<SyncEngine>(new (std::nothrow) SyncEngine(store_, outbox_, fs_, http_, clock_, random_, cancel_, config_));
    return engine_ && engine_->begin();
  }

  std::string run(const std::string& cmd, const Args& a) {
    resetCommandPeak();
    std::string body;
    if (cmd == "status") {
      body = status(a);
    } else if (cmd == "gate") {
      const SessionKind kind = arg(a, "kind") == "open"     ? SessionKind::Open
                               : arg(a, "kind") == "sleep" ? SessionKind::Sleep
                                                           : SessionKind::Manual;
      body = "\"gate\":" + esc(gateName(engine_->gate(kind)));
    } else if (cmd == "md5") {
      body = md5(a);
    } else if (cmd == "pair-start") {
      body = pairStart(a);
    } else if (cmd == "pair-poll") {
      body = pairPoll(a);
    } else if (cmd == "forget-credential") {
      heap::Scope scope;
      const bool ok = store_.forgetCredential();
      body = std::string("\"forgotten\":") + boolText(ok);
    } else if (cmd == "book-opened") {
      BookRef book = bookRef(a);
      heap::Scope scope;
      body = std::string("\"ok\":") + boolText(engine_->onBookOpened(book.id));
    } else if (cmd == "finalize-book") {
      body = finalize(a);
    } else if (cmd == "open-session") {
      body = openSession(a);
    } else if (cmd == "apply-choice") {
      body = applyChoice(a);
    } else if (cmd == "abandon-choice") {
      SessionReport report;
      {
        heap::Scope scope;
        report = engine_->abandonChoice();
      }
      body = "\"report\":" + reportJson(report);
    } else if (cmd == "sleep-session") {
      SessionReport report;
      {
        heap::Scope scope;
        report = engine_->runSleepSession();
      }
      body = "\"report\":" + reportJson(report);
    } else if (cmd == "manual-session") {
      body = manualSession(a);
    } else if (cmd == "catalog") {
      body = catalog(a);
    } else if (cmd == "catalog-cached") {
      body = catalogCached(a);
    } else if (cmd == "download") {
      body = download(a);
    } else if (cmd == "on-sd") {
      body = onSd(a);
    } else if (cmd == "remote-head") {
      body = remoteHead(a);
    } else if (cmd == "undo") {
      body = undo(a);
    } else if (cmd == "outbox") {
      body = outboxDump();
    } else if (cmd == "set-clock") {
      clock_.wallOffset = argInt(a, "wall_offset", clock_.wallOffset);
      if (hasArg(a, "precision")) clock_.precision = parsePrecision(arg(a, "precision"));
      if (hasArg(a, "mono_bump_ms")) clock_.monoOffset += static_cast<uint64_t>(argInt(a, "mono_bump_ms", 0));
      body = "\"clock_set\":true";
    } else if (cmd == "sizes") {
      std::ostringstream o;
      o << "\"sizes\":{\"store\":" << sizeof(Store) << ",\"outbox\":" << sizeof(Outbox) << ",\"engine\":" << sizeof(SyncEngine)
        << ",\"catalog\":" << sizeof(Catalog) << ",\"pairing_flow\":" << sizeof(PairingFlow)
        << ",\"remote_event\":" << sizeof(RemoteEvent) << ",\"book_ledger\":" << sizeof(BookLedger) << ",\"batch\":" << sizeof(Batch) << "}";
      body = o.str();
    } else {
      return "{\"ok\":false,\"error\":\"unknown command\",\"cmd\":" + esc(cmd) + "}";
    }
    std::ostringstream o;
    o << "{\"ok\":true,\"cmd\":" << esc(cmd);
    if (!body.empty()) o << "," << body;
    o << ",\"heap\":{\"live\":" << heap::live << ",\"peak\":" << heap::peak << ",\"process_peak\":" << processPeak_
      << ",\"allocations\":" << heap::allocations << ",\"largest\":" << heap::largest << "}"
      << ",\"fs\":{\"peak_open\":" << fs_.peakOpen() << ",\"exhausted\":" << fs_.exhausted() << ",\"still_open\":" << fs_.stillOpen() << "}"
      << ",\"http\":{\"requests\":" << http_.requests << ",\"connections\":" << http_.connections << "}}";
    if (heap::peak > processPeak_) processPeak_ = heap::peak;
    return o.str();
  }

 private:
  void resetCommandPeak() {
    if (heap::peak > processPeak_) processPeak_ = heap::peak;
    heap::peak = heap::live;
    heap::allocations = 0;
    heap::largest = 0;
  }

  BookRef bookRef(const Args& a) {
    BookRef book;
    if (hasArg(a, "md5")) {
      book.id.setHex(arg(a, "md5").c_str());
    } else {
      heap::Scope scope;
      partialMd5(fs_, arg(a, "file").c_str(), book.id);
    }
    bookTitle_ = arg(a, "title");
    bookAuthor_ = arg(a, "author");
    bookFile_ = arg(a, "book_file");
    if (bookFile_.empty() && hasArg(a, "file")) {
      const std::string path = arg(a, "file");
      bookFile_ = path.substr(path.rfind('/') + 1);
    }
    book.title = bookTitle_.empty() ? nullptr : bookTitle_.c_str();
    book.author = bookAuthor_.empty() ? nullptr : bookAuthor_.c_str();
    book.file = bookFile_.empty() ? nullptr : bookFile_.c_str();
    book.totalPages = static_cast<int32_t>(argInt(a, "total", -1));
    return book;
  }

  static Position positionFrom(const Args& a, const char* prefix = "") {
    Position p;
    const std::string pre = prefix;
    if (hasArg(a, (pre + "pct").c_str())) p.setPercent(strtod(arg(a, (pre + "pct").c_str()).c_str(), nullptr));
    p.page = static_cast<int32_t>(argInt(a, (pre + "page").c_str(), -1));
    p.totalPages = static_cast<int32_t>(argInt(a, (pre + "total").c_str(), argInt(a, "total", -1)));
    copyText(p.xpointer, sizeof(p.xpointer), arg(a, (pre + "xp").c_str()).c_str());
    return p;
  }

  std::string status(const Args&) {
    EngineStatus st = engine_->status();
    const Credentials& c = store_.creds();
    const SyncState& s = store_.state();
    std::ostringstream o;
    o << "\"paired\":" << boolText(st.paired) << ",\"needs_relink\":" << boolText(st.needsRelink) << ",\"depth\":" << st.depth
      << ",\"held\":" << st.held << ",\"cursor\":\"" << st.cursor << "\",\"committed_cursor\":\"" << s.committedCursor
      << "\",\"last_success_epoch\":" << st.lastSuccessEpoch << ",\"next_attempt_epoch\":" << st.nextAttemptEpoch
      << ",\"failures\":" << static_cast<int>(s.failures) << ",\"skipped_events\":" << st.skippedEvents
      << ",\"rejected_events\":" << st.rejectedEvents << ",\"last_rejection\":" << esc(s.lastRejection)
      << ",\"quarantined\":" << st.quarantinedRecords << ",\"diag\":" << diagJson(st.diag) << ",\"device_id\":" << esc(c.deviceId)
      << ",\"account_id\":" << esc(c.accountId) << ",\"base_url\":" << esc(c.baseUrl) << ",\"external_id\":" << esc(externalId_)
      << ",\"token_present\":" << boolText(c.token[0] != '\0') << ",\"expires_at\":" << c.expiresAtEpoch
      << ",\"renew_after\":" << c.renewAfterEpoch << ",\"holds\":" << static_cast<int>(s.holds.size())
      << ",\"next_sequence\":\"" << outbox_.nextSequence() << "\",\"clock\":{\"epoch\":" << clock_.wallNow().epochSec
      << ",\"precision\":" << esc(precisionName(clock_.wallNow().precision)) << ",\"set_calls\":" << clock_.setCalls << "}";
    return o.str();
  }

  std::string md5(const Args& a) {
    BookId id;
    bool ok;
    {
      heap::Scope scope;
      ok = partialMd5(fs_, arg(a, "file").c_str(), id);
    }
    return std::string("\"has_identity\":") + boolText(ok) + ",\"md5\":" + esc(id.hex);
  }

  PairingFlow& flow() {
    if (!flow_) {
      heap::Scope scope;
      pairing_.platform = "crossink-x4";
      pairing_.externalId = externalId_.c_str();
      pairing_.deviceName = opt_.deviceName.c_str();
      pairing_.firmwareVersion = opt_.firmware.c_str();
      pairing_.clientVersion = opt_.client.c_str();
      flow_ = std::unique_ptr<PairingFlow>(new PairingFlow(store_, outbox_, fs_, http_, clock_, random_, cancel_, pairing_));
    }
    return *flow_;
  }

  std::string pairingJson(bool started) {
    PairingFlow& f = flow();
    std::ostringstream o;
    o << "\"started\":" << boolText(started) << ",\"state\":" << esc(pairingName(f.state())) << ",\"user_code\":" << esc(f.userCode())
      << ",\"qr_url\":" << esc(f.qrUrl()) << ",\"interval\":" << f.intervalSec() << ",\"rate_limited\":" << boolText(f.rateLimited())
      << ",\"error\":" << diagJson(f.lastError()) << ",\"device_id\":" << esc(store_.creds().deviceId)
      << ",\"account_id\":" << esc(store_.creds().accountId) << ",\"paired\":" << boolText(store_.creds().paired())
      << ",\"expires_at\":" << store_.creds().expiresAtEpoch << ",\"renew_after\":" << store_.creds().renewAfterEpoch;
    return o.str();
  }

  std::string pairStart(const Args&) {
    PairingFlow& f = flow();
    bool started;
    {
      heap::Scope scope;
      started = f.start();
    }
    return pairingJson(started);
  }

  std::string pairPoll(const Args& a) {
    PairingFlow& f = flow();
    if (f.state() == PairingState::Idle && store_.creds().pairing.active()) {
      heap::Scope scope;
      f.start();
    }
    // No real waiting: the clock moves to when the flow says a request is due.
    const uint64_t now = clock_.monotonicMs();
    if (f.nextPollAtMs() > now) clock_.monoOffset += f.nextPollAtMs() - now;
    clock_.monoOffset += static_cast<uint64_t>(argInt(a, "bump_ms", 0));
    {
      heap::Scope scope;
      f.step();
    }
    return pairingJson(true);
  }

  std::string finalize(const Args& a) {
    FinalizeInput input;
    input.book = bookRef(a);
    input.position = positionFrom(a);
    input.paged = argInt(a, "paged", 1) != 0;
    input.lastTurn.epochSec = argInt(a, "turn_epoch", clock_.wallNow().epochSec);
    input.lastTurn.precision = hasArg(a, "turn_precision") ? parsePrecision(arg(a, "turn_precision")) : clock_.wallNow().precision;
    if (hasArg(a, "turn_mono")) {
      input.hasLastTurnMonotonic = true;
      input.lastTurnMonotonicMs = static_cast<uint64_t>(argInt(a, "turn_mono", 0));
    }
    SessionSummary session;
    if (hasArg(a, "session_start")) {
      const TimePrecision p = hasArg(a, "session_precision") ? parsePrecision(arg(a, "session_precision")) : TimePrecision::Exact;
      session.started = WallTime{argInt(a, "session_start", 0), p};
      session.ended = WallTime{argInt(a, "session_end", 0), p};
      session.pagesRead = static_cast<int32_t>(argInt(a, "pages_read", 0));
      session.startPage = static_cast<int32_t>(argInt(a, "start_page", -1));
      session.endPage = static_cast<int32_t>(argInt(a, "end_page", -1));
      input.session = &session;
    }
    std::unique_ptr<TsvClips> clips;
    if (hasArg(a, "clips")) {
      clips = std::make_unique<TsvClips>(arg(a, "clips"));
      input.clips = clips.get();
    }
    FinalizeResult r;
    {
      heap::Scope scope;
      r = engine_->finalizeBook(input);
    }
    std::ostringstream o;
    o << "\"finalize\":{\"ok\":" << boolText(r.ok) << ",\"progress_queued\":" << boolText(r.progressQueued)
      << ",\"session_queued\":" << boolText(r.sessionQueued) << ",\"annotations_created\":" << r.annotationsCreated
      << ",\"annotations_deleted\":" << r.annotationsDeleted << ",\"annotations_pending\":" << boolText(r.annotationsPending) << "}"
      << ",\"book\":" << esc(input.book.id.hex);
    return o.str();
  }

  std::string openSession(const Args& a) {
    const BookRef book = bookRef(a);
    Position local = positionFrom(a);
    SessionReport report;
    uint64_t started = GetTickCount64();
    {
      heap::Scope scope;
      report = engine_->runOpenSession(book, local, decision_);
    }
    const uint64_t elapsed = GetTickCount64() - started;
    localBefore_ = local;
    std::ostringstream o;
    o << "\"report\":" << reportJson(report) << ",\"decision\":" << decisionJson(decision_) << ",\"elapsed_ms\":" << elapsed;
    return o.str();
  }

  std::string applyChoice(const Args& a) {
    const bool go = arg(a, "choice") == "go";
    Position after = localBefore_;
    if (go) {
      // What the glue does: resolve the proposed position locally and report where it landed.
      after = Position{};
      after.hasPct = decision_.hasRemotePct;
      after.pctMicro = decision_.remotePct;
      copyText(after.xpointer, sizeof(after.xpointer), decision_.remoteXpointer);
      after.page = static_cast<int32_t>(argInt(a, "after_page", -1));
      after.totalPages = static_cast<int32_t>(argInt(a, "total", localBefore_.totalPages));
    }
    SessionReport report;
    {
      heap::Scope scope;
      report = engine_->applyChoice(decision_, go ? ResumeChoice::GoThere : ResumeChoice::StayHere, localBefore_, after,
                                    argInt(a, "online", 1) != 0);
    }
    return "\"report\":" + reportJson(report);
  }

  std::string manualSession(const Args& a) {
    SessionReport report;
    uint64_t started = GetTickCount64();
    if (hasArg(a, "file") || hasArg(a, "md5")) {
      const BookRef book = bookRef(a);
      Position local = positionFrom(a);
      {
        heap::Scope scope;
        report = engine_->runManualSession(&book, &local, &decision_);
      }
      localBefore_ = local;
    } else {
      heap::Scope scope;
      report = engine_->runManualSession(nullptr, nullptr, nullptr);
    }
    const uint64_t elapsed = GetTickCount64() - started;
    std::ostringstream o;
    o << "\"report\":" << reportJson(report) << ",\"decision\":" << decisionJson(decision_) << ",\"elapsed_ms\":" << elapsed;
    return o.str();
  }

  Catalog& catalogObject() {
    if (!catalog_) {
      heap::Scope scope;
      catalog_ = std::unique_ptr<Catalog>(new Catalog(store_, fs_, http_, clock_, cancel_));
    }
    return *catalog_;
  }

  class Collect : public ICatalogVisitor {
   public:
    void onBook(uint16_t, const CatalogEntry& entry) override { entries.push_back(entry); }
    std::vector<CatalogEntry> entries;
  };

  static std::string entriesJson(const std::vector<CatalogEntry>& entries) {
    std::ostringstream o;
    o << "[";
    for (size_t i = 0; i < entries.size(); ++i) {
      const CatalogEntry& e = entries[i];
      if (i) o << ",";
      o << "{\"id\":" << esc(e.id) << ",\"md5\":" << esc(e.book.hex) << ",\"sha256\":" << esc(e.sha256) << ",\"size\":" << e.fileSize
        << ",\"title\":" << esc(e.title) << ",\"author\":" << esc(e.author) << ",\"file_name\":" << esc(e.fileName) << "}";
    }
    o << "]";
    return o.str();
  }

  std::string catalog(const Args& a) {
    Catalog& c = catalogObject();
    CatalogPage page;
    CatalogResult r;
    {
      heap::Scope scope;
      r = c.fetchPage(static_cast<uint16_t>(argInt(a, "page", 1)), static_cast<uint16_t>(argInt(a, "limit", 20)), page);
    }
    Collect collect;
    CatalogPage cached;
    bool loaded = false;
    if (r == CatalogResult::Ok) loaded = c.loadPage(page.page, cached, collect);
    std::ostringstream o;
    o << "\"result\":" << esc(catalogName(r)) << ",\"page\":" << page.page << ",\"count\":" << page.count
      << ",\"has_next\":" << boolText(page.hasNext) << ",\"has_previous\":" << boolText(page.hasPrevious)
      << ",\"cache_loaded\":" << boolText(loaded) << ",\"books\":" << entriesJson(collect.entries) << ",\"error\":" << diagJson(c.lastError());
    return o.str();
  }

  std::string catalogCached(const Args& a) {
    Catalog& c = catalogObject();
    Collect collect;
    CatalogPage page;
    const bool loaded = c.loadPage(static_cast<uint16_t>(argInt(a, "page", 1)), page, collect);
    std::ostringstream o;
    o << "\"cache_loaded\":" << boolText(loaded) << ",\"has_next\":" << boolText(page.hasNext) << ",\"books\":" << entriesJson(collect.entries);
    return o.str();
  }

  bool findCached(const Args& a, CatalogEntry& out) {
    Collect collect;
    CatalogPage page;
    if (!catalogObject().loadPage(static_cast<uint16_t>(argInt(a, "page", 1)), page, collect)) return false;
    for (const CatalogEntry& e : collect.entries) {
      if (arg(a, "id") == e.id) {
        out = e;
        return true;
      }
    }
    return false;
  }

  class Progress : public IDownloadProgress {
   public:
    void onProgress(uint64_t received, uint64_t) override { last = received; }
    uint64_t last = 0;
  };

  std::string download(const Args& a) {
    CatalogEntry entry;
    if (!findCached(a, entry)) return "\"result\":\"not_in_cache\"";
    fs_.mkdir(arg(a, "dir", "/books").c_str());
    const bool before = catalogObject().isOnSd(entry, arg(a, "dir", "/books").c_str());
    Progress progress;
    DownloadResult r;
    {
      heap::Scope scope;
      r = catalogObject().download(entry, arg(a, "dir", "/books").c_str(), &progress, static_cast<uint32_t>(argInt(a, "budget_ms", 120000)));
    }
    std::ostringstream o;
    o << "\"result\":" << esc(downloadName(r)) << ",\"was_on_sd\":" << boolText(before)
      << ",\"on_sd\":" << boolText(catalogObject().isOnSd(entry, arg(a, "dir", "/books").c_str())) << ",\"file_name\":" << esc(entry.fileName)
      << ",\"received\":" << progress.last << ",\"error\":" << diagJson(catalogObject().lastError());
    return o.str();
  }

  std::string onSd(const Args& a) {
    CatalogEntry entry;
    if (!findCached(a, entry)) return "\"result\":\"not_in_cache\"";
    return std::string("\"on_sd\":") + boolText(catalogObject().isOnSd(entry, arg(a, "dir", "/books").c_str()));
  }

  std::string remoteHead(const Args& a) {
    const BookRef book = bookRef(a);
    RemoteHead head;
    const bool ok = engine_->remoteHead(book.id, head);
    std::ostringstream o;
    o << "\"has_head\":" << boolText(ok);
    if (ok) {
      o << ",\"server_seq\":\"" << head.serverSeq << "\",\"position\":" << positionJson(head.position)
        << ",\"device\":" << esc(head.deviceName) << ",\"occurred_at\":" << esc(head.occurredAt);
    }
    return o.str();
  }

  std::string undo(const Args& a) {
    const BookRef book = bookRef(a);
    std::ostringstream o;
    const bool has = engine_->hasUndo(book.id);
    o << "\"has_undo\":" << boolText(has);
    if (has && argInt(a, "take", 0) != 0) {
      Position p;
      const bool took = engine_->takeUndo(book.id, p);
      o << ",\"taken\":" << boolText(took) << ",\"position\":" << positionJson(p);
    }
    return o.str();
  }

  std::string outboxDump() {
    if (!outbox_.isOpen()) outbox_.open();
    std::ostringstream o;
    o << "\"depth\":" << outbox_.depth() << ",\"events\":[";
    bool first = true;
    for (size_t i = 0; i < outbox_.depth(); ++i) {
      const OutboxEntry* e = outbox_.entryAt(i);
      BatchItem item{};
      item.offset = e->offset;
      item.seq = e->seq;
      std::vector<char> buffer(e->payloadLen + 512);
      JsonWriter w(buffer.data(), buffer.size());
      w.arrayBegin();
      const auto result = outbox_.writeEnvelope(item, w);
      if (result != Outbox::EnvelopeResult::Ok) continue;
      w.arrayEnd();
      char id[37];
      uuidFormat(item.id, id);
      if (!first) o << ",";
      first = false;
      o << "{\"id\":\"" << id << "\",\"seq\":\"" << item.seq << "\",\"kind\":" << static_cast<int>(e->kind)
        << ",\"attempted\":" << boolText(e->attempted != 0) << ",\"envelope\":" << std::string(buffer.data(), w.size()) << "}";
    }
    o << "]";
    return o.str();
  }

  Options opt_;
  uint8_t mac_[6] = {0};
  HostFs fs_;
  HostClock clock_;
  HostRandom random_;
  NeverCancel cancel_;
  HostHttp http_;
  Store store_;
  Outbox outbox_;
  EngineConfig config_;
  std::unique_ptr<SyncEngine> engine_;
  std::unique_ptr<PairingFlow> flow_;
  PairingConfig pairing_;
  std::unique_ptr<Catalog> catalog_;
  ResumeDecision decision_;
  Position localBefore_;
  std::string externalId_;
  std::string bookTitle_, bookAuthor_, bookFile_;
  size_t processPeak_ = 0;
};

Args parseTokens(const std::vector<std::string>& tokens, size_t from) {
  Args a;
  for (size_t i = from; i < tokens.size(); ++i) {
    const size_t eq = tokens[i].find('=');
    if (eq == std::string::npos) {
      a[tokens[i]] = "1";
    } else {
      a[tokens[i].substr(0, eq)] = tokens[i].substr(eq + 1);
    }
  }
  return a;
}

}  // namespace

int main() {
  SetConsoleOutputCP(CP_UTF8);
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);

  // Arguments as UTF-8, whatever the ANSI code page is.
  int argcW = 0;
  wchar_t** argvW = CommandLineToArgvW(GetCommandLineW(), &argcW);
  std::vector<std::string> args;
  for (int i = 1; i < argcW; ++i) {
    char utf8[8192];
    WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, utf8, sizeof(utf8), nullptr, nullptr);
    args.emplace_back(utf8);
  }

  Options opt;
  size_t i = 0;
  const auto value = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
  for (; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a.rfind("--", 0) != 0) break;
    if (a == "--sd") opt.sd = value();
    else if (a == "--mac") opt.macHex = value();
    else if (a == "--server") opt.server = value();
    else if (a == "--device-name") opt.deviceName = value();
    else if (a == "--firmware") opt.firmware = value();
    else if (a == "--client") opt.client = value();
    else if (a == "--external-id") opt.externalId = value();
    else if (a == "--precision") opt.precision = value();
    else if (a == "--wall-offset") opt.wallOffset = _atoi64(value().c_str());
    else if (a == "--mono-step-ms") opt.monoStepMs = _atoi64(value().c_str());
    else if (a == "--open-budget-ms") opt.openBudgetMs = _atoi64(value().c_str());
    else if (a == "--sleep-budget-ms") opt.sleepBudgetMs = _atoi64(value().c_str());
    else if (a == "--manual-budget-ms") opt.manualBudgetMs = _atoi64(value().c_str());
    else if (a == "--max-body") opt.maxBodyBytes = _atoi64(value().c_str());
    else if (a == "--max-pull-pages") opt.maxPullPages = _atoi64(value().c_str());
    else if (a == "--pull-limit") opt.pullLimit = _atoi64(value().c_str());
    else if (a == "--event-parse-budget") opt.eventParseBudget = _atoi64(value().c_str());
    else if (a == "--script") opt.script = true;
    else {
      std::fprintf(stderr, "unknown option %s\n", a.c_str());
      return 64;
    }
  }
  if (opt.sd.empty()) {
    std::fprintf(stderr, "--sd is required\n");
    return 64;
  }

  auto device = std::make_unique<Device>(opt);
  if (!device->boot()) {
    std::cout << "{\"ok\":false,\"error\":\"boot failed\"}" << std::endl;
    return 2;
  }

  if (!opt.script) {
    if (i >= args.size()) {
      std::fprintf(stderr, "a command is required\n");
      return 64;
    }
    std::cout << device->run(args[i], parseTokens(args, i + 1)) << std::endl;
    return 0;
  }

  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    if (line == "exit") break;
    std::vector<std::string> tokens;
    size_t start = 0;
    for (;;) {
      const size_t tab = line.find('\t', start);
      tokens.push_back(line.substr(start, tab == std::string::npos ? tab : tab - start));
      if (tab == std::string::npos) break;
      start = tab + 1;
    }
    std::cout << device->run(tokens[0], parseTokens(tokens, 1)) << std::endl;
  }
  return 0;
}
