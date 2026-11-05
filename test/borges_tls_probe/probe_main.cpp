// TLS probe. Runs the device's own networking code (SDK SecureClient and
// SecureHttpClient, BorgesHttp, BorgesDns, BorgesClock, the anchors) against wolfSSL built
// with the device's configuration, over Winsock, against the real production host.
//
// It sends no credentials: only an unauthenticated GET of the capabilities route.
// Exit code 0 means every scenario did what it should. See build_probe.py.

#include <SecureHttpClient.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/memory.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "BorgesClock.h"
#include "BorgesDns.h"
#include "BorgesHttp.h"
#include "BorgesTrustAnchors.h"
#include "comodo_rsa_dv_ca.h"
#include "net/ClockChain.h"
#include "net/DnsChain.h"
#include "net/HttpDate.h"
#include "net/NetFailure.h"

using namespace borges;
using namespace borges::net;

// --- Time, which wolfSSL reads through these ------------------------------------------

static int64_t gClockOffset = 0;

extern "C" time_t probe_xtime(time_t* out) {
  const time_t now = time(nullptr) + static_cast<time_t>(gClockOffset);
  if (out) *out = now;
  return now;
}
extern "C" struct tm* probe_xgmtime(const time_t* when, struct tm* out) {
  gmtime_s(out, when);
  return out;
}

int64_t borgesProbeNow() { return static_cast<int64_t>(probe_xtime(nullptr)); }
void borgesProbeSetNow(const int64_t epoch) { gClockOffset = epoch - static_cast<int64_t>(time(nullptr)); }
bool gProbeFailGetaddrinfo = false;

// --- Heap, which wolfSSL allocates through these ---------------------------------------

struct HeapStats {
  size_t live = 0, peak = 0, largest = 0, calls = 0, liveBlocks = 0, blocksAtPeak = 0;
};
static HeapStats gHeap;

static void* trackedAlloc(const size_t size) {
  auto* block = static_cast<size_t*>(malloc(size + 8));
  if (block == nullptr) return nullptr;
  block[0] = size;
  gHeap.live += size;
  ++gHeap.liveBlocks;
  ++gHeap.calls;
  if (size > gHeap.largest) gHeap.largest = size;
  if (gHeap.live > gHeap.peak) {
    gHeap.peak = gHeap.live;
    gHeap.blocksAtPeak = gHeap.liveBlocks;
  }
  return reinterpret_cast<uint8_t*>(block) + 8;
}
static void trackedFree(void* ptr) {
  if (ptr == nullptr) return;
  auto* block = reinterpret_cast<size_t*>(static_cast<uint8_t*>(ptr) - 8);
  gHeap.live -= block[0];
  --gHeap.liveBlocks;
  free(block);
}
static void* trackedRealloc(void* ptr, const size_t size) {
  if (ptr == nullptr) return trackedAlloc(size);
  void* moved = trackedAlloc(size);
  if (moved == nullptr) return nullptr;
  const size_t old = reinterpret_cast<size_t*>(static_cast<uint8_t*>(ptr) - 8)[0];
  memcpy(moved, ptr, old < size ? old : size);
  trackedFree(ptr);
  return moved;
}
static void resetPeak() {
  gHeap.peak = gHeap.live;
  gHeap.largest = 0;
  gHeap.calls = 0;
  gHeap.blocksAtPeak = gHeap.liveBlocks;
}

// --- Stack: the scenarios run on a thread whose stack is painted first --------------------

static constexpr size_t kPaintBytes = 256 * 1024;
__attribute__((noinline)) static void paintStack() {
  volatile uint8_t area[kPaintBytes];
  for (size_t i = 0; i < kPaintBytes; ++i) area[i] = 0xA5;
}
__attribute__((noinline)) static size_t stackUsed() {
  volatile uint8_t area[kPaintBytes];
  size_t untouched = 0;
  while (untouched < kPaintBytes && area[untouched] == 0xA5) ++untouched;
  return kPaintBytes - untouched;
}

// --- Fakes ---------------------------------------------------------------------------

class TmpFs : public IFs {
 public:
  Handle open(const char* path, const Mode mode) override {
    if (mode == Mode::Read && files_.find(path) == files_.end()) return kInvalid;
    if (mode == Mode::WriteTrunc) files_[path].clear();
    Handle h = 0;
    while (h < 4 && !open_[h].path.empty()) ++h;
    if (h == 4) return kInvalid;
    open_[h].path = path;
    open_[h].pos = mode == Mode::Append ? files_[path].size() : 0;
    return h;
  }
  int32_t read(const Handle h, void* dst, const size_t len) override {
    std::string& file = files_[open_[h].path];
    const size_t n = std::min(len, file.size() - std::min(open_[h].pos, file.size()));
    memcpy(dst, file.data() + open_[h].pos, n);
    open_[h].pos += n;
    return static_cast<int32_t>(n);
  }
  int32_t write(const Handle h, const void* src, const size_t len) override {
    std::string& file = files_[open_[h].path];
    file.resize(std::max(file.size(), open_[h].pos + len));
    memcpy(file.data() + open_[h].pos, src, len);
    open_[h].pos += len;
    return static_cast<int32_t>(len);
  }
  bool seek(const Handle h, const uint32_t pos) override {
    open_[h].pos = pos;
    return true;
  }
  int64_t size(const Handle h) override { return static_cast<int64_t>(files_[open_[h].path].size()); }
  bool flush(Handle) override { return true; }
  void close(const Handle h) override { open_[h].path.clear(); }
  bool exists(const char* path) override { return files_.count(path) != 0; }
  bool remove(const char* path) override {
    files_.erase(path);
    return true;
  }
  bool rename(const char* from, const char* to) override {
    files_[to] = files_[from];
    files_.erase(from);
    return true;
  }
  bool mkdir(const char*) override { return true; }

 private:
  struct Open {
    std::string path;
    size_t pos = 0;
  };
  std::map<std::string, std::string> files_;
  Open open_[4];
};

class NeverCancel : public ICancel {
 public:
  bool cancelled() override { return false; }
};

class CountdownCancel : public ICancel {
 public:
  explicit CountdownCancel(const int polls) : polls_(polls) {}
  bool cancelled() override { return polls_-- <= 0; }

 private:
  int polls_;
};

class Collect : public IHttpSink {
 public:
  void onStatus(const int s) override { status = s; }
  bool onBody(const uint8_t* data, const size_t len) override {
    body.append(reinterpret_cast<const char*>(data), len);
    return true;
  }
  int status = 0;
  std::string body;
};

// --- Harness -------------------------------------------------------------------------

static const char* kProd = "borges.runadev.com";
static const char* kUrl = "https://borges.runadev.com/api/sync/v2/capabilities";
static int gFailures = 0;

static void expect(const bool ok, const char* what) {
  printf("    %s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++gFailures;
}
static void title(const char* name) { printf("\n[%s]\n", name); }

struct Outcome {
  int status = 0;
  int failStage = 0;
  int failCode = 0;
  size_t liveWhileConnected = 0;
  std::string version, cipher, curve, body, date;
};

static const char* failStageName(const int stage) {
  static const char* names[] = {"none", "tcp", "alloc", "trust_load", "handshake", "timeout", "aborted"};
  return names[stage];
}

static const char* wolfName(const int code) {
  if (code == 0) return "-";
  static char text[WOLFSSL_MAX_ERROR_SZ];
  return wolfSSL_ERR_error_string(static_cast<unsigned long>(code < 0 ? -code : code), text);
}

// The SDK client with chosen anchors. Dials by name unless ip is given.
static Outcome fetch(const char* url, const char* const* anchors, const size_t count) {
  Outcome o;
  freeink::SecureHttpClient http;
  http.setTrustAnchors(anchors, count);
  http.setUserAgent("borges-tls-probe");
  http.setTimeout(12000);
  http.begin(url);
  http.addHeader("Accept", "application/json");
  o.status = http.sendRequest("GET", nullptr, 0, [&o](const uint8_t* d, size_t n) {
    o.body.append(reinterpret_cast<const char*>(d), n);
    return true;
  });
  auto* ssl = static_cast<WOLFSSL*>(http.secureClient().nativeHandle());
  if (ssl != nullptr) {
    o.version = wolfSSL_get_version(ssl);
    o.cipher = wolfSSL_get_cipher_name(ssl);
    const char* curve = wolfSSL_get_curve_name(ssl);
    o.curve = curve ? curve : "(none reported)";
  }
  o.liveWhileConnected = gHeap.live;
  o.failStage = static_cast<int>(http.secureClient().lastFailStage());
  o.failCode = http.secureClient().lastFailCode();
  http.end();
  return o;
}

static void show(const Outcome& o) {
  printf("    status=%d fail_stage=%s code=%d (%s)\n", o.status, failStageName(o.failStage), o.failCode, wolfName(o.failCode));
}

static void refused(const char* name, const Outcome& o, const int code) {
  title(name);
  show(o);
  expect(o.status <= 0, "no HTTP status came back");
  expect(o.failStage == static_cast<int>(freeink::SecureClient::FailStage::Handshake) && o.failCode == code,
         ("handshake refused with " + std::string(wolfName(code))).c_str());
}

static void measured(const char* name, const char* const* anchors, const size_t count) {
  title(name);
  ProbeNet::resetCounters();
  resetPeak();
  const Outcome o = fetch(kUrl, anchors, count);
  show(o);
  printf("    tls=%s cipher=%s curve=%s\n", o.version.c_str(), o.cipher.c_str(), o.curve.c_str());
  printf("    inbound: %u records, largest %u bytes, %u bytes total; outbound %u bytes\n",
         static_cast<unsigned>(ProbeNet::recordsIn), static_cast<unsigned>(ProbeNet::maxRecordIn),
         static_cast<unsigned>(ProbeNet::bytesIn), static_cast<unsigned>(ProbeNet::bytesOut));
  printf("    wolfSSL heap: peak %u bytes in %u blocks, largest block %u, %u allocations; held while connected %u; live after close %u\n",
         static_cast<unsigned>(gHeap.peak), static_cast<unsigned>(gHeap.blocksAtPeak),
         static_cast<unsigned>(gHeap.largest), static_cast<unsigned>(gHeap.calls),
         static_cast<unsigned>(o.liveWhileConnected), static_cast<unsigned>(gHeap.live));
  expect(o.status == 401, "the server answered 401");
  expect(o.body.rfind("{\"error\"", 0) == 0, "with a JSON error body");
}

// --- Scenarios -------------------------------------------------------------------------

static void costScenarios() {
  const char* const* all = trustAnchors();

  title("cost of loading the anchors, with no connection");
  for (size_t count : {size_t{1}, kTrustAnchorCount}) {
    resetPeak();
    WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
    for (size_t i = 0; i < count; ++i) {
      wolfSSL_CTX_load_verify_buffer(ctx, reinterpret_cast<const unsigned char*>(all[i]), strlen(all[i]), WOLFSSL_FILETYPE_PEM);
    }
    printf("    %u anchor(s): peak %u, largest block %u, held by the context %u\n", static_cast<unsigned>(count),
           static_cast<unsigned>(gHeap.peak), static_cast<unsigned>(gHeap.largest), static_cast<unsigned>(gHeap.live));
    wolfSSL_CTX_free(ctx);
  }

  title("stack of one verified request, painted just for it");
  paintStack();
  const Outcome o = fetch(kUrl, all, kTrustAnchorCount);
  const size_t used = stackUsed();
  printf("    status=%d, %u bytes of stack below the caller (32-bit x86 frames, not RISC-V)\n", o.status, static_cast<unsigned>(used));
  expect(o.status == 401, "the server answered 401");

  // A larger public page, because a small response says nothing about record sizes.
  title("record sizes on a large response (the public landing page, unauthenticated)");
  ProbeNet::resetCounters();
  resetPeak();
  const Outcome page = fetch("https://borges.runadev.com/", all, kTrustAnchorCount);
  printf("    status=%d body=%u bytes; inbound %u records, largest %u bytes\n", page.status, static_cast<unsigned>(page.body.size()),
         static_cast<unsigned>(ProbeNet::recordsIn), static_cast<unsigned>(ProbeNet::maxRecordIn));
  printf("    wolfSSL heap: peak %u, largest block %u, held while connected %u\n", static_cast<unsigned>(gHeap.peak),
         static_cast<unsigned>(gHeap.largest), static_cast<unsigned>(page.liveWhileConnected));
  expect(page.status > 0 && page.status < 500, "the page came back");

  if (const char* url = getenv("BORGES_PROBE_LARGE_URL")) {
    title("record sizes on a large static file");
    ProbeNet::resetCounters();
    resetPeak();
    const Outcome large = fetch(url, all, kTrustAnchorCount);
    printf("    status=%d body=%u bytes; inbound %u records, largest %u bytes\n", large.status,
           static_cast<unsigned>(large.body.size()), static_cast<unsigned>(ProbeNet::recordsIn),
           static_cast<unsigned>(ProbeNet::maxRecordIn));
    printf("    wolfSSL heap: peak %u, largest block %u, held while connected %u\n", static_cast<unsigned>(gHeap.peak),
           static_cast<unsigned>(gHeap.largest), static_cast<unsigned>(large.liveWhileConnected));
  }
}

static void trustScenarios() {
  const char* const* all = trustAnchors();

  measured("production, all five anchors", all, kTrustAnchorCount);
  for (size_t i = 0; i < kTrustAnchorCount; ++i) {
    if (strncmp(trustAnchorName(i), "ISRG", 4) != 0) continue;
    measured((std::string("production, only ") + trustAnchorName(i)).c_str(), all + i, 1);
  }

  refused("production, no anchors at all", fetch(kUrl, nullptr, 0), wolfcode::kNoSigner);
  const char* const gtsOnly[] = {all[3], all[4]};
  refused("production, only the anchors of another CA (GTS)", fetch(kUrl, gtsOnly, 2), wolfcode::kNoSigner);

  refused("expired.badssl.com, configured anchors (its CA is not one of ours)",
          fetch("https://expired.badssl.com/", all, kTrustAnchorCount), wolfcode::kNoSigner);
  const char* const expiredCa[] = {kComodoRsaDvCa};
  refused("expired.badssl.com, trusting its issuer: the date alone must refuse it",
          fetch("https://expired.badssl.com/", expiredCa, 1), wolfcode::kAfterDate);
  refused("wrong.host.badssl.com, configured anchors: chains to X1, name is wrong",
          fetch("https://wrong.host.badssl.com/", all, kTrustAnchorCount), wolfcode::kNameMismatch);
}

static void wrongNameScenario() {
  // SNI is the real host, so the server hands out its real certificate; only the name to
  // check differs. The SDK ties the two together, so this one call sequence is spelled out.
  title("production certificate, checked against another name");
  WiFiClient transport;
  transport.setConnectionTimeout(5000);
  expect(transport.connect(kProd, 443) == 1, "TCP connected");
  WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
  wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER | WOLFSSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
  for (size_t i = 0; i < kTrustAnchorCount; ++i) {
    wolfSSL_CTX_load_verify_buffer(ctx, reinterpret_cast<const unsigned char*>(trustAnchors()[i]),
                                   strlen(trustAnchors()[i]), WOLFSSL_FILETYPE_PEM);
  }
  wolfSSL_SetIORecv(ctx, [](WOLFSSL*, char* buf, int sz, void* c) -> int {
    auto* t = static_cast<WiFiClient*>(c);
    if (t->available() == 0) return t->connected() ? WOLFSSL_CBIO_ERR_WANT_READ : WOLFSSL_CBIO_ERR_CONN_CLOSE;
    const int n = t->read(reinterpret_cast<uint8_t*>(buf), sz);
    return n > 0 ? n : WOLFSSL_CBIO_ERR_WANT_READ;
  });
  wolfSSL_SetIOSend(ctx, [](WOLFSSL*, char* buf, int sz, void* c) -> int {
    const size_t n = static_cast<WiFiClient*>(c)->write(reinterpret_cast<const uint8_t*>(buf), sz);
    return n > 0 ? static_cast<int>(n) : WOLFSSL_CBIO_ERR_CONN_CLOSE;
  });
  WOLFSSL* ssl = wolfSSL_new(ctx);
  wolfSSL_SetIOReadCtx(ssl, &transport);
  wolfSSL_SetIOWriteCtx(ssl, &transport);
  wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, kProd, strlen(kProd));
  wolfSSL_check_domain_name(ssl, "not-borges.example.com");
  wolfSSL_UseKeyShare(ssl, WOLFSSL_ECC_X25519);
  wolfSSL_UseMaxFragment(ssl, WOLFSSL_MFL_2_11);
  int err = 0;
  const unsigned long deadline = millis() + 12000;
  for (;;) {
    if (wolfSSL_connect(ssl) == WOLFSSL_SUCCESS) break;
    err = wolfSSL_get_error(ssl, 0);
    if (err != WOLFSSL_ERROR_WANT_READ && err != WOLFSSL_ERROR_WANT_WRITE) break;
    if (millis() > deadline) break;
    delay(5);
  }
  printf("    code=%d (%s)\n", err, wolfName(err));
  expect(err == wolfcode::kNameMismatch, "refused with the name mismatch");
  wolfSSL_free(ssl);
  wolfSSL_CTX_free(ctx);
}

static void anchorLoadScenario() {
  title("one damaged anchor among good ones");
  std::string damaged = trustAnchors()[1];
  // A byte in the middle of the base64, as a bad flash or a bad edit would leave it.
  damaged[damaged.size() / 2] = '!';
  const char* const list[] = {trustAnchors()[0], damaged.c_str(), trustAnchors()[2]};
  const Outcome o = fetch(kUrl, list, 3);
  show(o);
  expect(o.status <= 0, "no request went out");
  expect(o.failStage == static_cast<int>(freeink::SecureClient::FailStage::TrustLoad), "reported as trust_load");

  // Why they are loaded one by one: the same damage inside one blob goes unnoticed.
  std::string blob = std::string(trustAnchors()[0]) + damaged + trustAnchors()[2];
  WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
  const int ret = wolfSSL_CTX_load_verify_buffer(ctx, reinterpret_cast<const unsigned char*>(blob.data()), blob.size(),
                                                 WOLFSSL_FILETYPE_PEM);
  printf("    the same three as one PEM blob: wolfSSL_CTX_load_verify_buffer returned %d (1 is success)\n", ret);
  wolfSSL_CTX_free(ctx);
}

static void flowScenarios() {
  NeverCancel never;

  {
    title("BorgesHttp: keep-alive on one TLS connection, then a dropped socket (rule 2)");
    TmpFs fs;
    BorgesDns dns(fs);
    BorgesClock clock(dns, kProd);
    clock.begin(0);
    BorgesHttp http(fs, dns, &clock, "borges-tls-probe");
    HttpRequest request;
    request.url = kUrl;
    ProbeNet::resetCounters();
    for (int i = 0; i < 3; ++i) {
      Collect sink;
      HttpResponse out;
      http.perform(request, sink, never, out);
      printf("    request %d: status=%d complete=%d connects=%d id=%s\n", i + 1, out.status, out.complete, ProbeNet::connects,
             out.requestId);
      expect(out.status == 401 && out.complete, "401, complete");
      expect(sink.status == 401, "the sink saw the status first");
      expect(out.requestId[0] != '\0', "X-Request-Id captured");
    }
    expect(ProbeNet::connects == 1, "three requests, one connection");
    ProbeNet::dropLive();
    Collect sink;
    HttpResponse out;
    http.perform(request, sink, never, out);
    printf("    after the drop: status=%d connects=%d\n", out.status, ProbeNet::connects);
    expect(out.status == 401 && ProbeNet::connects == 2, "reconnected once and answered");
    // Dropped again, and this time nothing answers: a failure, promptly, not a loop.
    ProbeNet::dropLive();
    ProbeNet::blocked[2] = 443;
    Collect refusedSink;
    HttpResponse refusedOut;
    http.perform(request, refusedSink, never, refusedOut);
    const char* refusedWord = failureCode(refusedOut.stage, refusedOut.transportCode, TimePrecision::Exact);
    printf("    dropped and refused: stage=%d code=%d word=%s\n", static_cast<int>(refusedOut.stage), refusedOut.transportCode, refusedWord);
    expect(strcmp(refusedWord, "tcp_failed") == 0, "tcp_failed");
    ProbeNet::blocked[2] = 0;
    Collect againSink;
    HttpResponse againOut;
    http.perform(request, againSink, never, againOut);
    expect(againOut.status == 401, "and the next request starts over and works");
    http.disconnect();
  }

  {
    title("BorgesHttp: untrusted peer becomes the word the screen shows");
    TmpFs fs;
    BorgesDns dns(fs);
    BorgesClock clock(dns, kProd);
    clock.begin(0);
    BorgesHttp http(fs, dns, &clock, "borges-tls-probe");
    HttpRequest request;
    request.url = "https://expired.badssl.com/";
    Collect sink;
    HttpResponse out;
    http.perform(request, sink, never, out);
    const char* word = failureCode(out.stage, out.transportCode, TimePrecision::Unknown);
    printf("    stage=%d code=%d word=%s\n", static_cast<int>(out.stage), out.transportCode, word);
    expect(strcmp(word, "tls_untrusted") == 0, "tls_untrusted");
    expect(out.status == 0 && sink.body.empty(), "nothing reached the sink");
  }

  {
    title("BorgesHttp: heap gate");
    TmpFs fs;
    BorgesDns dns(fs);
    BorgesClock clock(dns, kProd);
    clock.begin(0);
    BorgesHttp http(fs, dns, &clock, "borges-tls-probe");
    ESP.freeHeap = 40 * 1024;
    HttpRequest request;
    request.url = kUrl;
    Collect sink;
    HttpResponse out;
    ProbeNet::resetCounters();
    http.perform(request, sink, never, out);
    ESP.freeHeap = 200 * 1024;
    const char* word = failureCode(out.stage, out.transportCode, TimePrecision::Unknown);
    printf("    stage=%d code=%d word=%s connects=%d\n", static_cast<int>(out.stage), out.transportCode, word, ProbeNet::connects);
    expect(strcmp(word, "low_memory") == 0 && ProbeNet::connects == 0, "low_memory, without touching the network");
  }

  {
    title("BorgesHttp: cancel during the handshake");
    TmpFs fs;
    BorgesDns dns(fs);
    BorgesClock clock(dns, kProd);
    clock.begin(0);
    BorgesHttp http(fs, dns, &clock, "borges-tls-probe");
    CountdownCancel cancel(8);
    HttpRequest request;
    request.url = kUrl;
    Collect sink;
    HttpResponse out;
    http.perform(request, sink, cancel, out);
    printf("    stage=%d status=%d\n", static_cast<int>(out.stage), out.status);
    expect(out.stage == TransportStage::Aborted && out.status == 0, "aborted, nothing delivered");
  }

  const int64_t realNow = borgesProbeNow();

  {
    title("Clock behind the leaf (stale anchor): repaired from the HTTP Date, then trusted");
    TmpFs fs;
    BorgesDns dns(fs);
    const int64_t anchor = kMinTrustedEpoch + 86400;  // 2026-01-02, before today's leaf was issued.
    borgesProbeSetNow(anchor);
    BorgesClock clock(dns, kProd);
    clock.begin(anchor);
    clock.setWall(anchor, TimePrecision::Anchored);
    BorgesHttp http(fs, dns, &clock, "borges-tls-probe");
    HttpRequest request;
    request.url = kUrl;
    Collect sink;
    HttpResponse out;
    http.perform(request, sink, never, out);
    const WallTime after = clock.wallNow();
    printf("    status=%d clock moved by %lld s to label=%d\n", out.status, static_cast<long long>(after.epochSec - anchor),
           static_cast<int>(after.precision));
    expect(out.status == 401, "the handshake succeeded on the retry");
    expect(after.precision == TimePrecision::Approximate, "port 80 earned the approximate label");
    const int64_t error = after.epochSec - realNow;
    expect(error > -60 && error < 60, "and the clock is right");
    http.disconnect();
    borgesProbeSetNow(realNow);
  }

  {
    title("Same, with port 80 filtered: repaired from HTTPS with verification off for one read");
    TmpFs fs;
    BorgesDns dns(fs);
    const int64_t anchor = kMinTrustedEpoch + 86400;
    borgesProbeSetNow(anchor);
    ProbeNet::blocked[0] = 80;
    BorgesClock clock(dns, kProd);
    clock.begin(anchor);
    clock.setWall(anchor, TimePrecision::Anchored);
    BorgesHttp http(fs, dns, &clock, "borges-tls-probe");
    HttpRequest request;
    request.url = kUrl;
    Collect sink;
    HttpResponse out;
    http.perform(request, sink, never, out);
    const WallTime after = clock.wallNow();
    printf("    status=%d label=%d\n", out.status, static_cast<int>(after.precision));
    expect(out.status == 401, "the handshake succeeded on the retry, verified");
    const int64_t error = after.epochSec - realNow;
    expect(error > -60 && error < 60, "and the clock is right");
    http.disconnect();
    ProbeNet::blocked[0] = 0;
    borgesProbeSetNow(realNow);
  }

  {
    title("Clock with nothing to repair it: tls_clock, in plain words");
    TmpFs fs;
    BorgesDns dns(fs);
    const int64_t anchor = kMinTrustedEpoch + 86400;
    borgesProbeSetNow(anchor);
    ProbeNet::blocked[0] = 80;
    BorgesClock clock(dns, kProd);
    clock.begin(anchor);
    clock.setWall(anchor, TimePrecision::Anchored);
    BorgesHttp http(fs, dns, nullptr, "borges-tls-probe");
    HttpRequest request;
    request.url = kUrl;
    Collect sink;
    HttpResponse out;
    http.perform(request, sink, never, out);
    const char* word = failureCode(out.stage, out.transportCode, clock.wallNow().precision);
    printf("    stage=%d code=%d word=%s\n", static_cast<int>(out.stage), out.transportCode, word);
    expect(strcmp(word, "tls_clock") == 0, "tls_clock");
    http.disconnect();
    ProbeNet::blocked[0] = 0;
    borgesProbeSetNow(realNow);
  }

  {
    title("DNS chain over the real network (rule 8)");
    TmpFs fs;
    {
      BorgesDns dns(fs);
      const Candidates& c = dns.resolve(kProd, 6000, never);
      printf("    getaddrinfo: count=%d first=%u.%u.%u.%u source=%d\n", c.count, c.items[0].ip[0], c.items[0].ip[1],
             c.items[0].ip[2], c.items[0].ip[3], static_cast<int>(c.items[0].source));
      expect(c.count > 0 && c.items[0].source == DnsSource::Getaddrinfo, "answered by the system resolver");
    }
    gProbeFailGetaddrinfo = true;
    {
      BorgesDns dns(fs);
      const Candidates& c = dns.resolve(kProd, 6000, never);
      printf("    dns over tcp: count=%d first=%u.%u.%u.%u source=%d\n", c.count, c.items[0].ip[0], c.items[0].ip[1],
             c.items[0].ip[2], c.items[0].ip[3], static_cast<int>(c.items[0].source));
      expect(c.count > 0 && c.items[0].source == DnsSource::DnsOverTcp, "answered over TCP by a public resolver");
    }
    ProbeNet::blocked[1] = 53;
    {
      BorgesDns dns(fs);
      BorgesClock clock(dns, kProd);
      clock.begin(0);
      BorgesHttp http(fs, dns, &clock, "borges-tls-probe");
      const Candidates& c = dns.resolve(kProd, 6000, never);
      printf("    all dns dead: count=%d first=%u.%u.%u.%u source=%d\n", c.count, c.items[0].ip[0], c.items[0].ip[1],
             c.items[0].ip[2], c.items[0].ip[3], static_cast<int>(c.items[0].source));
      expect(c.count == 2 && c.items[0].source == DnsSource::Baked, "the baked addresses come out");
      HttpRequest request;
      request.url = kUrl;
      Collect sink;
      HttpResponse out;
      http.perform(request, sink, never, out);
      printf("    dialed the baked address: status=%d\n", out.status);
      expect(out.status == 401, "verified against the hostname, and answered");
      http.disconnect();
      const Candidates& after = dns.resolve(kProd, 6000, never);
      expect(after.items[0].source != DnsSource::Getaddrinfo, "still no system resolver");
    }
    {
      BorgesDns dns(fs);
      const Candidates& c = dns.resolve(kProd, 6000, never);
      expect(c.items[0].source == DnsSource::LastGood, "the address that worked is now the last good one");
    }
    gProbeFailGetaddrinfo = false;
    ProbeNet::blocked[1] = 0;
    {
      BorgesDns dns(fs);
      const Candidates& c = dns.resolve("nonexistent.borges.invalid", 6000, never);
      printf("    unknown host: count=%d\n", c.count);
      expect(c.count == 0, "another host gets no baked address: dns_failed");
    }
  }
}

// OTA keeps using setInsecure() for the firmware bytes (pinned by a digest) and mbedTLS for
// the manifest, so it depends on none of the strict path. This shows that the new build
// flags leave the insecure path able to reach the hosts it uses.
static void otaPathScenario() {
  for (const char* url : {"https://api.github.com/", "https://github.com/", "https://release-assets.githubusercontent.com/"}) {
    title(url);
    freeink::SecureHttpClient http;
    http.setInsecure();
    http.setUserAgent("borges-tls-probe");
    http.setTimeout(12000);
    http.begin(url);
    const int status = http.sendRequest("GET", nullptr, 0, [](const uint8_t*, size_t) { return false; });
    auto* ssl = static_cast<WOLFSSL*>(http.secureClient().nativeHandle());
    printf("    status=%d %s %s\n", status, ssl ? wolfSSL_get_version(ssl) : "-", ssl ? wolfSSL_get_cipher_name(ssl) : "-");
    http.end();
    expect(status > 0, "reached it over the insecure path");
  }
}

static DWORD WINAPI runAll(void*) {
  if (getenv("BORGES_PROBE_ONLY_PROD") != nullptr) {
    // The counterfactual builds: does production still work without this flag?
    title("production, all five anchors");
    show(fetch(kUrl, trustAnchors(), kTrustAnchorCount));
    for (size_t i = 0; i < 3; ++i) {
      title((std::string("production, only ") + trustAnchorName(i)).c_str());
      show(fetch(kUrl, trustAnchors() + i, 1));
    }
    return 0;
  }
  costScenarios();
  paintStack();
  trustScenarios();
  wrongNameScenario();
  anchorLoadScenario();
  flowScenarios();
  otaPathScenario();
  const size_t used = stackUsed();
  printf("\nstack high-water across all scenarios on this 32-bit build: %u bytes\n", static_cast<unsigned>(used));
  return 0;
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  wolfSSL_SetAllocators(trackedAlloc, trackedFree, trackedRealloc);
  wolfSSL_Init();

  printf("wolfSSL %s, probe build %d-bit, sizeof(long)=%u\n", wolfSSL_lib_version(), static_cast<int>(sizeof(void*) * 8),
         static_cast<unsigned>(sizeof(long)));
  for (size_t i = 0; i < kTrustAnchorCount; ++i) {
    printf("anchor %u: %s, %u PEM bytes\n", static_cast<unsigned>(i), trustAnchorName(i),
           static_cast<unsigned>(strlen(trustAnchors()[i])));
  }

  HANDLE thread = CreateThread(nullptr, kPaintBytes * 2, runAll, nullptr, 0, nullptr);
  WaitForSingleObject(thread, INFINITE);

  printf("\n%s: %d failure%s\n", gFailures ? "FAILED" : "PASSED", gFailures, gFailures == 1 ? "" : "s");
  return gFailures ? 1 : 0;
}
