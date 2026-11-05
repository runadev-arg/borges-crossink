// Borges on emulated RISC-V: the same wolfSSL, SecureNet, BorgesCore and src/borges/net
// the firmware links, on the same prebuilt SDK, with QEMU's OpenCores MAC instead of the
// radio and LittleFS on flash instead of the SD card. Nothing here is product code.
//
// What it measures is the memory and the stack of the code that runs on the device. It
// says nothing about time: QEMU's CPU is not a 160 MHz C3. See run.py for how to run it.
//
// The runner writes a plan into the last 64 KB of app1, as "key=value" lines, so a new
// scenario needs no rebuild. Every line of interest starts with "@@".

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_eth.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_netif.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <fcntl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <string>

#include "Outbox.h"
#include "Store.h"
#include "SyncEngine.h"
#include "borges/net/BorgesClock.h"
#include "borges/net/BorgesDevice.h"
#include "borges/net/BorgesDns.h"
#include "borges/net/BorgesHttp.h"
#include "hooks.h"
#include "net/DnsChain.h"
#include "net/NetFailure.h"

// The real anchors, under another name, so the local server's test root can take one
// slot at run time. kTrustAnchorCount stays 5, so the load costs what it costs on device.
#define trustAnchors realTrustAnchors
#define trustAnchorName realTrustAnchorName
#include "borges/net/BorgesTrustAnchors.cpp"
#undef trustAnchors
#undef trustAnchorName

namespace {
const char* gAnchors[borges::net::kTrustAnchorCount];
bool gAnchorsReady = false;
void ensureAnchors() {
  if (gAnchorsReady) return;
  for (size_t i = 0; i < borges::net::kTrustAnchorCount; ++i) gAnchors[i] = borges::net::realTrustAnchors()[i];
  gAnchorsReady = true;
}
}  // namespace

namespace borges {
namespace net {
const char* const* trustAnchors() {
  ensureAnchors();
  return gAnchors;
}
const char* trustAnchorName(size_t index) { return realTrustAnchorName(index); }
}  // namespace net
}  // namespace borges

using namespace borges;

extern "C" esp_eth_mac_t* esp_eth_mac_new_openeth(const eth_mac_config_t* config);  // See openeth_shim.c.

namespace {

constexpr const char* kProdUrl = "https://borges.runadev.com/api/sync/v2/capabilities";
constexpr const char* kLocalHttp = "http://10.0.2.2:8080";
constexpr const char* kLocalHttps = "https://10.0.2.2:8443";
constexpr const char* kBookMd5 = "0123456789abcdef0123456789abcdef";

std::map<std::string, std::string> gPlan;
long planInt(const char* key, const long fallback) {
  const auto it = gPlan.find(key);
  return it == gPlan.end() ? fallback : strtol(it->second.c_str(), nullptr, 0);
}
std::string planStr(const char* key, const char* fallback) {
  const auto it = gPlan.find(key);
  return it == gPlan.end() ? fallback : it->second;
}

void readPlan() {
  const esp_partition_t* part =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  if (part == nullptr) return;
  char text[512];
  if (esp_partition_read(part, part->size - 0x10000, text, sizeof(text) - 1) != ESP_OK) return;
  text[sizeof(text) - 1] = '\0';
  char* line = text;
  while (line != nullptr && *line != '\0' && *line != '\xff') {
    char* next = strchr(line, '\n');
    if (next != nullptr) *next++ = '\0';
    char* eq = strchr(line, '=');
    if (eq != nullptr) {
      *eq = '\0';
      gPlan[line] = eq + 1;
    }
    line = next;
  }
}

// --- Measurement ------------------------------------------------------------------------

void phaseBegin() {
  gMaxRequest = 0;
  gFailedAllocs = 0;
  gLargestFailed = 0;
  gBigCount = 0;
  gTraceBig = true;
  heap_caps_monitor_local_minimum_free_size_stop();
  heap_caps_monitor_local_minimum_free_size_start();
}

uint32_t gTaskStackBytes = 0;

// Numbers first, printing after: printf's own frames must not count unless they belong.
void phaseEnd(const char* name, const bool showBig = false) {
  const uint32_t hwm = gTaskStackBytes ? uxTaskGetStackHighWaterMark(nullptr) : 0;
  const uint32_t freeNow = ESP.getFreeHeap();
  const uint32_t largest = ESP.getMaxAllocHeap();
  const uint32_t phaseMin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  gTraceBig = false;
  printf("@@ %-18s t=%-7lu free=%-6u largest=%-6u phase_min=%-6u max_req=%-6u failed=%u/%u stack_hwm=%u stack_peak=%u\n",
         name, static_cast<unsigned long>(millis()), static_cast<unsigned>(freeNow), static_cast<unsigned>(largest),
         static_cast<unsigned>(phaseMin), static_cast<unsigned>(gMaxRequest), static_cast<unsigned>(gFailedAllocs),
         static_cast<unsigned>(gLargestFailed), static_cast<unsigned>(hwm),
         static_cast<unsigned>(gTaskStackBytes ? gTaskStackBytes - hwm : 0));
  if (showBig) {
    for (uint32_t i = 0; i < gBigCount; ++i) {
      printf("@@   big size=%-6u free_before=%-6u largest_before=%-6u ra=%p %s\n", static_cast<unsigned>(gBig[i].size),
             static_cast<unsigned>(gBig[i].freeBefore), static_cast<unsigned>(gBig[i].largestBefore), gBig[i].ra,
             gBig[i].ok ? "ok" : "FAILED");
    }
  }
  fflush(stdout);
}

void printTasks() {
  UBaseType_t count = uxTaskGetNumberOfTasks();
  auto* tasks = static_cast<TaskStatus_t*>(malloc(count * sizeof(TaskStatus_t)));
  if (tasks == nullptr) return;
  count = uxTaskGetSystemState(tasks, count, nullptr);
  for (UBaseType_t i = 0; i < count; ++i) {
    printf("@@   task %-16s prio=%-2u stack_hwm=%u\n", tasks[i].pcTaskName, static_cast<unsigned>(tasks[i].uxBasePriority),
           static_cast<unsigned>(tasks[i].usStackHighWaterMark));
  }
  free(tasks);
}

// --- Network ------------------------------------------------------------------------------

esp_netif_t* gNetif = nullptr;
std::atomic<bool> gGotIp{false};

void onGotIp(void*, esp_event_base_t, int32_t, void* data) {
  const auto* event = static_cast<ip_event_got_ip_t*>(data);
  printf("@@ dhcp ip=" IPSTR " gw=" IPSTR "\n", IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw));
  gGotIp = true;
}

bool bringUpEthernet() {
  esp_netif_init();
  const esp_err_t loop = esp_event_loop_create_default();
  if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) return false;
  esp_netif_config_t netifConfig = ESP_NETIF_DEFAULT_ETH();
  gNetif = esp_netif_new(&netifConfig);
  eth_mac_config_t macConfig = ETH_MAC_DEFAULT_CONFIG();
  eth_phy_config_t phyConfig = ETH_PHY_DEFAULT_CONFIG();
  phyConfig.phy_addr = 1;
  phyConfig.reset_gpio_num = -1;
  phyConfig.autonego_timeout_ms = 100;
  esp_eth_mac_t* mac = esp_eth_mac_new_openeth(&macConfig);
  esp_eth_phy_t* phy = esp_eth_phy_new_dp83848(&phyConfig);
  if (mac == nullptr || phy == nullptr) {
    printf("@@ eth: mac=%p phy=%p\n", mac, phy);
    return false;
  }
  esp_eth_config_t ethConfig = ETH_DEFAULT_CONFIG(mac, phy);
  esp_eth_handle_t handle = nullptr;
  esp_err_t err = esp_eth_driver_install(&ethConfig, &handle);
  if (err != ESP_OK) {
    printf("@@ eth install: %s\n", esp_err_to_name(err));
    return false;
  }
  uint8_t address[6] = {0x02, 0x42, 0x4F, 0x52, 0x47, 0x53};
  esp_eth_ioctl(handle, ETH_CMD_S_MAC_ADDR, address);
  esp_netif_attach(gNetif, esp_eth_new_netif_glue(handle));
  esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, onGotIp, nullptr);
  err = esp_eth_start(handle);
  if (err != ESP_OK) {
    printf("@@ eth start: %s\n", esp_err_to_name(err));
    return false;
  }
  for (int i = 0; i < 300 && !gGotIp; ++i) delay(100);
  return gGotIp;
}

void setResolver(const uint8_t a, const uint8_t b, const uint8_t c, const uint8_t d) {
  esp_netif_dns_info_t dns{};
  dns.ip.type = ESP_IPADDR_TYPE_V4;
  dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(a, b, c, d);
  esp_netif_set_dns_info(gNetif, ESP_NETIF_DNS_MAIN, &dns);
  dns.ip.u_addr.ip4.addr = 0;
  esp_netif_set_dns_info(gNetif, ESP_NETIF_DNS_BACKUP, &dns);
}

// --- IFs over LittleFS. The SD card's stand-in: same contract as BorgesFsHal --------------

class LfsFs : public IFs {
 public:
  Handle open(const char* path, const Mode mode) override {
    char full[160];
    snprintf(full, sizeof(full), "/lfs%s", path);
    int flags = O_RDONLY;
    if (mode == Mode::WriteTrunc) flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (mode == Mode::Append) flags = O_WRONLY | O_CREAT | O_APPEND;
    const int fd = ::open(full, flags, 0644);
    return fd < 0 ? kInvalid : static_cast<Handle>(fd);
  }
  int32_t read(const Handle h, void* dst, const size_t len) override { return ::read(h, dst, len); }
  int32_t write(const Handle h, const void* src, const size_t len) override { return ::write(h, src, len); }
  bool seek(const Handle h, const uint32_t pos) override { return ::lseek(h, pos, SEEK_SET) >= 0; }
  int64_t size(const Handle h) override {
    struct stat st{};
    return ::fstat(h, &st) == 0 ? st.st_size : -1;
  }
  bool flush(const Handle h) override { return ::fsync(h) == 0; }
  void close(const Handle h) override { ::close(h); }
  bool exists(const char* path) override {
    char full[160];
    snprintf(full, sizeof(full), "/lfs%s", path);
    struct stat st{};
    return ::stat(full, &st) == 0;
  }
  bool remove(const char* path) override {
    char full[160];
    snprintf(full, sizeof(full), "/lfs%s", path);
    return ::unlink(full) == 0 || !exists(path);
  }
  bool rename(const char* from, const char* to) override {
    char a[160], b[160];
    snprintf(a, sizeof(a), "/lfs%s", from);
    snprintf(b, sizeof(b), "/lfs%s", to);
    return ::rename(a, b) == 0;
  }
  bool mkdir(const char* path) override {
    char full[160];
    snprintf(full, sizeof(full), "/lfs%s", path);
    return ::mkdir(full, 0755) == 0 || exists(path);
  }
};

// IFs on static RAM: fast, and the SD card has no heap cost either. fs=lfs selects LittleFS.
class RamFs : public IFs {
 public:
  Handle open(const char* path, const Mode mode) override {
    Node* node = find(path);
    if (mode == Mode::Read && node == nullptr) return kInvalid;
    if (node == nullptr) node = create(path);
    if (node == nullptr) return kInvalid;
    for (int i = 0; i < kHandles; ++i) {
      if (handles_[i].node != nullptr) continue;
      if (mode == Mode::WriteTrunc) node->size = 0;
      handles_[i].node = node;
      handles_[i].pos = mode == Mode::Append ? node->size : 0;
      handles_[i].append = mode == Mode::Append;
      return static_cast<Handle>(i);
    }
    return kInvalid;
  }
  int32_t read(const Handle h, void* dst, const size_t len) override {
    OpenFile& f = handles_[h];
    const size_t left = f.pos < f.node->size ? f.node->size - f.pos : 0;
    const size_t n = len < left ? len : left;
    memcpy(dst, f.node->data + f.pos, n);
    f.pos += n;
    return static_cast<int32_t>(n);
  }
  int32_t write(const Handle h, const void* src, const size_t len) override {
    OpenFile& f = handles_[h];
    if (f.append) f.pos = f.node->size;
    if (f.pos + len > f.node->capacity) {
      printf("@@ ramfs full: %s\n", f.node->path);
      return -1;
    }
    memcpy(f.node->data + f.pos, src, len);
    f.pos += len;
    if (f.pos > f.node->size) f.node->size = f.pos;
    return static_cast<int32_t>(len);
  }
  bool seek(const Handle h, const uint32_t pos) override {
    handles_[h].pos = pos;
    return true;
  }
  int64_t size(const Handle h) override { return handles_[h].node->size; }
  bool flush(Handle) override { return true; }
  void close(const Handle h) override { handles_[h].node = nullptr; }
  bool exists(const char* path) override { return find(path) != nullptr; }
  bool remove(const char* path) override {
    Node* node = find(path);
    if (node != nullptr) {
      node->path[0] = '\0';
      node->size = 0;
    }
    return true;
  }
  bool rename(const char* from, const char* to) override {
    Node* node = find(from);
    if (node == nullptr || find(to) != nullptr) return false;
    snprintf(node->path, sizeof(node->path), "%s", to);
    return true;
  }
  bool mkdir(const char*) override { return true; }
  // A session's files, up front, so that they never land in the middle of a shaped heap.
  void reserve() {
    for (uint8_t*& pool : bigPool_) {
      if (pool == nullptr) pool = static_cast<uint8_t*>(malloc(kBigBytes));
    }
  }

 private:
  static constexpr int kHandles = 8;
  static constexpr int kBig = 4;
  static constexpr int kSmall = 16;
  static constexpr size_t kBigBytes = 24 * 1024;
  static constexpr size_t kSmallBytes = 1536;
  struct Node {
    char path[80] = {0};
    uint8_t* data = nullptr;
    size_t capacity = 0;
    size_t size = 0;
  };
  struct OpenFile {
    Node* node = nullptr;
    size_t pos = 0;
    bool append = false;
  };
  static bool wantsBig(const char* path) {
    return strstr(path, "spool") != nullptr || strstr(path, ".log") != nullptr || strstr(path, ".bad") != nullptr ||
           strstr(path, "inbox") != nullptr;
  }
  Node* find(const char* path) {
    for (Node& node : nodes_) {
      if (node.path[0] != '\0' && strcmp(node.path, path) == 0) return &node;
    }
    return nullptr;
  }
  Node* create(const char* path) {
    static uint8_t smallPool[kSmall][kSmallBytes];
    const bool big = wantsBig(path);
    const int first = big ? 0 : kBig;
    const int last = big ? kBig : kBig + kSmall;
    for (int i = first; i < last; ++i) {
      Node& node = nodes_[i];
      if (node.path[0] != '\0') continue;
      if (big && bigPool_[i] == nullptr) bigPool_[i] = static_cast<uint8_t*>(malloc(kBigBytes));
      node.data = big ? bigPool_[i] : smallPool[i - kBig];
      if (node.data == nullptr) return nullptr;
      node.capacity = big ? kBigBytes : kSmallBytes;
      node.size = 0;
      snprintf(node.path, sizeof(node.path), "%s", path);
      return &node;
    }
    printf("@@ ramfs out of files: %s\n", path);
    return nullptr;
  }
  Node nodes_[kBig + kSmall];
  OpenFile handles_[kHandles];
  uint8_t* bigPool_[kBig] = {nullptr, nullptr, nullptr, nullptr};
};

RamFs gRamFs;
LfsFs gLfsFs;

// Mirrors the deadline half of glue::SessionCancel.
class PlanCancel : public ICancel {
 public:
  bool cancelled() override { return deadlineMs_ != 0 && millis() >= deadlineMs_; }
  void arm(const uint32_t budgetMs) { deadlineMs_ = budgetMs ? millis() + budgetMs : 0; }

 private:
  uint32_t deadlineMs_ = 0;
};

// The members of glue::BorgesStack that a sync session uses, in its order and with the
// same heap block. BorgesLink, Catalog and PairingFlow are not here.
struct TestStack {
  char clientVersion[48] = "crossink-borges/qemu";
  uint8_t mac[6] = {0};
  IFs& fs;
  Store store;
  net::EspRandom random;
  Outbox outbox;
  net::BorgesDns dns;
  net::BorgesClock clock;
  PlanCancel cancel;
  net::BorgesHttp http;
  SyncEngine engine;

  static EngineConfig config(const char* version) {
    EngineConfig c;
    c.firmwareVersion = "qemu";
    c.clientVersion = version;
    return c;
  }
  explicit TestStack(IFs& files)
      : fs(files),
        store(fs, loadMac(mac), sizeof(mac)),
        outbox(fs, random, kOutboxDir),
        dns(fs),
        clock(dns, "borges.runadev.com"),
        http(fs, dns, &clock, clientVersion),
        engine(store, outbox, fs, http, clock, random, cancel, config(clientVersion)) {}

 private:
  static const uint8_t* loadMac(uint8_t* out) {
    net::readChipMac(out);
    return out;
  }
};

class CountingSink : public IHttpSink {
 public:
  void onStatus(const int status) override { statusSeen = status; }
  bool onBody(const uint8_t* data, const size_t len) override {
    (void)data;
    bytes += len;
    if (keep != nullptr && kept + len < keepCap) {
      memcpy(keep + kept, data, len);
      kept += len;
      keep[kept] = '\0';
    }
    return true;
  }
  int statusSeen = 0;
  size_t bytes = 0;
  char* keep = nullptr;
  size_t keepCap = 0;
  size_t kept = 0;
};

const char* stageName(const TransportStage s) {
  switch (s) {
    case TransportStage::None:
      return "none";
    case TransportStage::Dns:
      return "dns";
    case TransportStage::Connect:
      return "connect";
    case TransportStage::Tls:
      return "tls";
    case TransportStage::Send:
      return "send";
    case TransportStage::ReadHeaders:
      return "read_headers";
    case TransportStage::ReadBody:
      return "read_body";
    case TransportStage::Aborted:
      return "aborted";
  }
  return "?";
}

const char* qemuPrecision(const TimePrecision p) {
  switch (p) {
    case TimePrecision::Unknown:
      return "unknown";
    case TimePrecision::Approximate:
      return "approximate";
    case TimePrecision::Anchored:
      return "anchored";
    case TimePrecision::Exact:
      return "exact";
  }
  return "?";
}

const char* sourceName(const net::DnsSource s) {
  switch (s) {
    case net::DnsSource::Getaddrinfo:
      return "getaddrinfo";
    case net::DnsSource::DnsOverTcp:
      return "dns_over_tcp";
    case net::DnsSource::LastGood:
      return "last_good";
    case net::DnsSource::Baked:
      return "baked";
  }
  return "?";
}

void printCandidates(const char* label, const net::Candidates& c, const uint32_t ms) {
  printf("@@ %s in %lu ms, %u candidates:", label, static_cast<unsigned long>(ms), static_cast<unsigned>(c.count));
  for (uint8_t i = 0; i < c.count; ++i) {
    printf(" %u.%u.%u.%u(%s)", c.items[i].ip[0], c.items[i].ip[1], c.items[i].ip[2], c.items[i].ip[3],
           sourceName(c.items[i].source));
  }
  printf("\n");
}

const char* outcomeName(const SessionOutcome o) {
  static const char* const names[] = {"ok",           "awaiting_choice", "not_paired",  "cancelled",
                                      "budget",       "needs_relink",    "rate_limited", "transport",
                                      "server_error", "protocol_error",  "local_error", "stale_choice"};
  return names[static_cast<int>(o)];
}

void printReport(const char* label, const SessionReport& r, const SyncEngine& engine) {
  const Diagnostics& d = engine.diagnostics();
  printf("@@ %s outcome=%s pages=%u applied=%u skipped=%u sent=%u acked=%u rejected=%u quarantined=%u depth=%u diag=%s\n",
         label, outcomeName(r.outcome), r.pagesPulled, r.eventsApplied, r.eventsSkipped, r.eventsSent, r.eventsAcked,
         r.eventsRejected, r.eventsQuarantined, static_cast<unsigned>(r.depth), d.code);
}

// --- Ballast: shapes the heap into what a loaded device has ----------------------------

constexpr size_t kMaxBallast = 96;
void* gBallast[kMaxBallast];
size_t gBallastCount = 0;

void releaseBallast() {
  for (size_t i = 0; i < gBallastCount; ++i) free(gBallast[i]);
  gBallastCount = 0;
}

// Leaves `target` bytes free, in one piece when hole == 0, otherwise in holes of `hole`
// bytes separated by small pinned allocations.
void shapeHeap(const uint32_t target, const uint32_t hole) {
  releaseBallast();
  if (hole == 0) {
    // One contiguous block of `target` bytes and nothing else: hide the largest block, fill
    // every other, give the largest back, trim it. The best shape a heap can have, so a
    // failure here is a floor and not bad luck.
    void* whole = malloc(ESP.getMaxAllocHeap() - 64);
    if (whole == nullptr) return;
    while (gBallastCount + 1 < kMaxBallast && ESP.getMaxAllocHeap() >= 128) {
      void* p = malloc(ESP.getMaxAllocHeap() - 64);
      if (p == nullptr) break;
      gBallast[gBallastCount++] = p;
    }
    free(whole);
    const uint32_t freeNow = ESP.getFreeHeap();
    if (freeNow > target + 32 && gBallastCount < kMaxBallast) {
      void* p = malloc(freeNow - target - 16);
      if (p != nullptr) gBallast[gBallastCount++] = p;
    }
    return;
  }
  // Blocks of `hole` bytes in a row, with whatever is smaller filled in first, then every
  // other block freed: neighbours are held, so holes cannot coalesce. A small pin between
  // them would not do it, the allocator would put the pin somewhere else.
  void* blocks[kMaxBallast];
  size_t blockCount = 0;
  while (blockCount < kMaxBallast && ESP.getMaxAllocHeap() >= hole + 64) {
    void* p = malloc(hole);
    if (p == nullptr) break;
    blocks[blockCount++] = p;
  }
  while (gBallastCount < kMaxBallast / 2 && ESP.getMaxAllocHeap() >= 48) {
    void* p = malloc(ESP.getMaxAllocHeap() - 32);
    if (p == nullptr) break;
    gBallast[gBallastCount++] = p;
  }
  size_t give = target / (hole + 8);
  for (size_t i = 0; i < blockCount; ++i) {
    if (i % 2 == 1 && give > 0) {
      free(blocks[i]);
      --give;
    } else if (gBallastCount < kMaxBallast) {
      gBallast[gBallastCount++] = blocks[i];
    } else {
      free(blocks[i]);
    }
  }
}

// --- Scenarios --------------------------------------------------------------------------

TestStack* gStack = nullptr;
char gLocalRoot[2048];
char gLocalX1[2600];
bool gHaveLocalX1 = false;

bool fetchPem(const char* path, char* out, const size_t cap) {
  CountingSink sink;
  sink.keep = out;
  sink.keepCap = cap - 1;
  std::string url = std::string(kLocalHttp) + path;
  HttpRequest request;
  request.url = url.c_str();
  HttpResponse response;
  gStack->cancel.arm(30000);
  gStack->http.perform(request, sink, gStack->cancel, response);
  gStack->http.disconnect();
  printf("@@ plain http %s -> status=%d stage=%s code=%ld bytes=%u\n", url.c_str(), response.status,
         stageName(response.stage), static_cast<long>(response.transportCode), static_cast<unsigned>(sink.bytes));
  return response.status == 200 && strstr(out, "BEGIN CERTIFICATE") != nullptr;
}

// Slots: 0 Root YE, 1 X2, 2 X1, 3 GTS R1, 4 GTS R4. The local chain's two roots take the
// last two of the ones production does not use. X1 stays RSA-4096 in its slot, as the test
// X1 is, because production's chain reaches it: the X2 cross certificate is signed by X1.
// drop_x1=1 leaves X1 out, to see what that verification costs.
bool fetchLocalRoot() {
  ensureAnchors();
  const bool ye = fetchPem("/root.pem", gLocalRoot, sizeof(gLocalRoot));
  const bool x1 = fetchPem("/x1.pem", gLocalX1, sizeof(gLocalX1));
  if (planInt("drop_x1", 0)) {
    gAnchors[2] = gAnchors[3];
    printf("@@ anchors: X1 left out\n");
  }
  if (ye) gAnchors[net::kTrustAnchorCount - 1] = gLocalRoot;
  gHaveLocalX1 = x1;
  return ye;
}

// Production keeps its own X1 until the scenario talks to the local server.
void useLocalX1() {
  if (gHaveLocalX1 && !planInt("drop_x1", 0)) gAnchors[2] = gLocalX1;
}

void runDnsScenario() {
  TestStack& s = *gStack;
  s.cancel.arm(0);

  // lwip_getaddrinfo in a task of the size BorgesDns gives it, to read its high-water mark.
  static std::atomic<int> gaiResult{-100};
  static std::atomic<uint32_t> gaiHwm{0};
  xTaskCreate(
      [](void*) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        const int err = lwip_getaddrinfo("runadev.com", nullptr, &hints, &found);
        if (found != nullptr) lwip_freeaddrinfo(found);
        gaiHwm = uxTaskGetStackHighWaterMark(nullptr);
        gaiResult = err;
        vTaskDelete(nullptr);
      },
      "gai-probe", 3072, nullptr, 1, nullptr);
  for (int i = 0; i < 600 && gaiResult == -100; ++i) delay(50);
  printf("@@ gai task: err=%d stack 3072, hwm=%u, peak=%u\n", gaiResult.load(), static_cast<unsigned>(gaiHwm.load()),
         static_cast<unsigned>(3072 - gaiHwm.load()));

  // Dead resolver first: once lwIP has the answer in its cache, getaddrinfo never asks.
  // gai-probe above resolved another name for this reason.
  phaseBegin();
  setResolver(10, 0, 2, 99);  // Nobody answers there: getaddrinfo must fail and DNS over TCP take over.
  uint32_t t = millis();
  {
    net::BorgesDns dns(s.fs);
    printCandidates("rule8 dead resolver", dns.resolve("borges.runadev.com", 8000, s.cancel), millis() - t);
  }
  setResolver(10, 0, 2, 3);
  phaseEnd("dns_dead_resolver");

  phaseBegin();
  t = millis();
  {
    net::BorgesDns dns(s.fs);
    printCandidates("rule8 live resolver", dns.resolve("borges.runadev.com", 6000, s.cancel), millis() - t);
  }
  phaseEnd("dns_live");
}

void printClock(const char* label, TestStack& s) {
  const WallTime now = s.clock.wallNow();
  printf("@@ %s epoch=%lld precision=%s\n", label, static_cast<long long>(now.epochSec), qemuPrecision(now.precision));
}

void runClockScenario() {
  TestStack& s = *gStack;
  printClock("clock at boot", s);

  // What bringUp() does: a tight budget, so Date first and SNTP last.
  phaseBegin();
  uint32_t t = millis();
  s.cancel.arm(0);
  const bool ensured = s.clock.ensure(4000, s.cancel);
  printf("@@ clock.ensure(4000) -> %d in %lu ms\n", ensured, static_cast<unsigned long>(millis() - t));
  printClock("clock after ensure", s);
  phaseEnd("clock_ensure");

  // A roomy budget starts from SNTP.
  phaseBegin();
  t = millis();
  const bool repaired = s.clock.repair(30000, s.cancel);
  printf("@@ clock.repair(30000) -> %d in %lu ms\n", repaired, static_cast<unsigned long>(millis() - t));
  printClock("clock after repair", s);
  phaseEnd("clock_repair_sntp");
}

void runProdScenario() {
  TestStack& s = *gStack;
  phaseBegin();
  const uint32_t t = millis();
  s.cancel.arm(30000);
  CountingSink first;
  HttpRequest request;
  request.url = kProdUrl;
  HttpResponse response;
  s.http.perform(request, first, s.cancel, response);
  printf("@@ prod GET capabilities -> status=%d complete=%d stage=%s code=%ld body=%u request_id=%s in %lu ms\n",
         response.status, response.complete, stageName(response.stage), static_cast<long>(response.transportCode),
         static_cast<unsigned>(first.bytes), response.requestId, static_cast<unsigned long>(millis() - t));
  phaseEnd("tls_prod_first", true);

  phaseBegin();
  CountingSink second;
  s.http.perform(request, second, s.cancel, response);
  printf("@@ prod keep-alive -> status=%d complete=%d stage=%s code=%ld body=%u\n", response.status, response.complete,
         stageName(response.stage), static_cast<long>(response.transportCode), static_cast<unsigned>(second.bytes));
  phaseEnd("tls_prod_reuse");

  phaseBegin();
  s.http.disconnect();
  phaseEnd("tls_prod_closed");
}

void seedOutbox(TestStack& s, const int count, const int padding) {
  BookId book;
  book.setHex(kBookMd5);
  for (int i = 0; i < count; ++i) {
    char body[900];
    char filler[640];
    memset(filler, 'x', sizeof(filler));
    const int fill = padding < static_cast<int>(sizeof(filler)) ? padding : static_cast<int>(sizeof(filler)) - 1;
    filler[fill] = '\0';
    const int len = snprintf(body, sizeof(body),
                             "\"event_version\":1,\"event_type\":\"annotation.created\",\"aggregate_type\":\"annotation\","
                             "\"aggregate_id\":\"a%d\",\"document_id\":null,\"book_identifier\":{\"kind\":"
                             "\"koreader_partial_md5\",\"value\":\"%s\"},\"occurred_at\":null,\"time_precision\":\"unknown\","
                             "\"payload\":{\"text\":\"subrayado %d\",\"filler\":\"%s\"}",
                             i, kBookMd5, i, filler);
    uint8_t raw[16];
    const auto appended = s.outbox.append(EventKind::Annotation, book.md5, body, len, false, raw, nullptr);
    if (appended != Outbox::AppendResult::Ok) printf("@@ seed %d failed: %d\n", i, static_cast<int>(appended));
  }
}

void runSessionScenario() {
  TestStack& s = *gStack;
  gRamFs.reserve();
  useLocalX1();
  phaseBegin();
  Credentials& creds = s.store.creds();
  snprintf(creds.baseUrl, sizeof(creds.baseUrl), "%s", kLocalHttps);
  snprintf(creds.deviceId, sizeof(creds.deviceId), "qemu-device");
  snprintf(creds.token, sizeof(creds.token), "local-fake-server-not-a-credential");
  s.store.saveCredentials();
  seedOutbox(s, planInt("events", 30), planInt("padding", 300));
  BookId id;
  id.setHex(kBookMd5);
  s.engine.onBookOpened(id);
  phaseEnd("session_seed");

  BookRef book;
  book.id = id;
  book.title = "Ficciones";
  book.author = "Borges";
  book.file = "ficciones.epub";
  Position local;
  local.setPercent(12.5);
  local.page = 40;
  local.totalPages = 320;
  ResumeDecision decision;

  // session_free: what a session is left with after everything else in the boot.
  const long sessionFree = planInt("session_free", 0);
  if (sessionFree != 0) {
    shapeHeap(sessionFree, planInt("session_hole", 0));
    printf("@@ session heap shaped: free=%u largest=%u gate=%d\n", static_cast<unsigned>(ESP.getFreeHeap()),
           static_cast<unsigned>(ESP.getMaxAllocHeap()),
           net::heapAllowsHandshake(ESP.getFreeHeap(), ESP.getMaxAllocHeap()));
  }

  phaseBegin();
  s.cancel.arm(0);
  uint32_t t = millis();
  SessionReport report = s.engine.runManualSession(&book, &local, &decision);
  printReport("manual session", report, s.engine);
  printf("@@ manual session took %lu ms, ask=%d\n", static_cast<unsigned long>(millis() - t), decision.ask);
  phaseEnd("session_manual", true);

  if (report.outcome == SessionOutcome::AwaitingChoice) {
    phaseBegin();
    t = millis();
    report = s.engine.applyChoice(decision, ResumeChoice::StayHere, local, local);
    printReport("apply choice", report, s.engine);
    printf("@@ apply choice took %lu ms\n", static_cast<unsigned long>(millis() - t));
    phaseEnd("session_choice", true);
  }

  phaseBegin();
  seedOutbox(s, 3, 100);
  report = s.engine.runSleepSession();
  printReport("sleep session", report, s.engine);
  phaseEnd("session_sleep");

  phaseBegin();
  s.http.disconnect();
  phaseEnd("session_closed");
  releaseBallast();
}

// The handshake without the gate in BorgesHttp, which is what the sweep is there to judge.
struct ProbeResult {
  int status = 0;
  int failStage = 0;
  int failCode = 0;
  size_t bytes = 0;
  bool complete = false;
};

ProbeResult probeOnce(const std::string& url, const uint8_t ip[4]) {
  ProbeResult out;
  auto* http = new (std::nothrow) freeink::SecureHttpClient();
  if (http == nullptr) return out;
  http->setTrustAnchors(net::trustAnchors(), net::kTrustAnchorCount);
  http->setUserAgent("crossink-borges/qemu");
  http->setTimeout(20000);
  http->setConnectTimeout(3500);
  http->setReuse(false);
  if (http->begin(url)) {
    http->setServerAddress(IPAddress(ip[0], ip[1], ip[2], ip[3]));
    size_t bytes = 0;
    out.status = http->sendRequest(
        "GET", nullptr, 0,
        [&bytes](const uint8_t*, const size_t len) {
          bytes += len;
          return true;
        },
        nullptr);
    out.bytes = bytes;
    out.complete = http->responseComplete();
    out.failStage = static_cast<int>(http->secureClient().lastFailStage());
    out.failCode = http->secureClient().lastFailCode();
  }
  http->end();
  delete http;
  return out;
}

// sweep=<first>:<last>:<step>[,hole=N] against one target, descending. Stops after two
// failures in a row.
void runSweep() {
  const std::string target = planStr("sweep_target", "local");
  const bool prod = target == "prod";
  if (!prod) useLocalX1();
  const long first = planInt("sweep_first", 60000);
  const long last = planInt("sweep_last", 16000);
  const long step = planInt("sweep_step", 4000);
  const long hole = planInt("sweep_hole", 0);
  const long bodyBytes = planInt("sweep_body", 0);
  gSendMaxFragment = planInt("mfl", 1) != 0;
  const uint8_t localIp[4] = {10, 0, 2, 2};
  const uint8_t prodIp[4] = {104, 21, 0, 166};
  std::string url = prod ? std::string(kProdUrl) : std::string(kLocalHttps) + "/big?n=" + std::to_string(bodyBytes);
  printf("@@ sweep target=%s hole=%ld body=%ld mfl=%d url=%s\n", target.c_str(), hole, bodyBytes, gSendMaxFragment,
         url.c_str());
  int failures = 0;
  for (long level = first; level >= last && failures < 2; level -= step) {
    shapeHeap(level, hole);
    const uint32_t freeBefore = ESP.getFreeHeap();
    const uint32_t largestBefore = ESP.getMaxAllocHeap();
    const bool gate = net::heapAllowsHandshake(freeBefore, largestBefore);
    phaseBegin();
    const ProbeResult r = probeOnce(url, prod ? prodIp : localIp);
    const uint32_t phaseMin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t maxReq = gMaxRequest;
    const uint32_t failedAllocs = gFailedAllocs;
    const uint32_t largestFailed = gLargestFailed;
    gTraceBig = false;
    releaseBallast();
    const bool ok = r.status > 0 && r.complete && r.bytes == static_cast<size_t>(bodyBytes ? bodyBytes : r.bytes);
    printf("@@ sweep free=%-6u largest=%-6u gate=%d -> %s status=%d stage=%d code=%d bytes=%u used_peak=%u max_req=%u "
           "failed_allocs=%u largest_failed=%u\n",
           static_cast<unsigned>(freeBefore), static_cast<unsigned>(largestBefore), gate, ok ? "PASS" : "FAIL", r.status,
           r.failStage, r.failCode, static_cast<unsigned>(r.bytes), static_cast<unsigned>(freeBefore - phaseMin),
           static_cast<unsigned>(maxReq), static_cast<unsigned>(failedAllocs), static_cast<unsigned>(largestFailed));
    fflush(stdout);
    failures = ok ? 0 : failures + 1;
    delay(prod ? 1500 : 200);
  }
  printf("@@ sweep done, free=%u largest=%u\n", static_cast<unsigned>(ESP.getFreeHeap()),
         static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

volatile bool gSessionDone = false;

void sessionTask(void*) {
  gSendMaxFragment = planInt("mfl", 1) != 0;
  phaseBegin();
  const bool useFlash = planStr("fs", "ram") == "lfs";
  gStack = new (std::nothrow) TestStack(useFlash ? static_cast<IFs&>(gLfsFs) : static_cast<IFs&>(gRamFs));
  printf("@@ sizeof TestStack=%u, BorgesHttp=%u, SyncEngine=%u, Store=%u, Outbox=%u\n",
         static_cast<unsigned>(sizeof(TestStack)), static_cast<unsigned>(sizeof(net::BorgesHttp)),
         static_cast<unsigned>(sizeof(SyncEngine)), static_cast<unsigned>(sizeof(Store)),
         static_cast<unsigned>(sizeof(Outbox)));
  phaseEnd("stack_new");
  if (gStack == nullptr) {
    gSessionDone = true;
    vTaskDelete(nullptr);
  }

  phaseBegin();
  const bool store = gStack->store.begin();
  gStack->clock.begin(0);
  phaseEnd("store_begin");
  phaseBegin();
  const bool engine = gStack->engine.begin();
  printf("@@ store=%d engine=%d\n", store, engine);
  phaseEnd("engine_begin");

  const bool haveRoot = fetchLocalRoot();
  phaseEnd("plain_http");

  if (planInt("dns", 1)) runDnsScenario();
  if (planInt("clock", 1)) runClockScenario();
  if (planInt("prod", 1)) runProdScenario();
  if (planInt("session", 1) && haveRoot) runSessionScenario();
  if (gPlan.count("sweep_target") != 0) runSweep();

  phaseBegin();
  printTasks();
  phaseEnd("end");
  printf("@@ session task stack %u bytes, final hwm=%u, peak=%u\n", static_cast<unsigned>(gTaskStackBytes),
         static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
         static_cast<unsigned>(gTaskStackBytes - uxTaskGetStackHighWaterMark(nullptr)));
  printf("@@ DONE\n");
  fflush(stdout);
  gSessionDone = true;
  vTaskDelete(nullptr);
}

// What WiFi.mode(WIFI_STA) costs up to where QEMU's missing radio stops it. esp_phy_enable
// is stubbed (hooks.cpp), so the driver allocates all it allocates at start, and no frame
// ever moves. A watcher prints the heap, in case the call never comes back.
void wifiWatcher(void*) {
  for (int i = 0; i < 40; ++i) {
    printf("@@ wifi watch t=%lu free=%u largest=%u min=%u tasks=%u\n", static_cast<unsigned long>(millis()),
           static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()),
           static_cast<unsigned>(ESP.getMinFreeHeap()), static_cast<unsigned>(uxTaskGetNumberOfTasks()));
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  printf("@@ DONE\n");
  vTaskDelete(nullptr);
}

void runWifiProbe() {
  gTaskStackBytes = 0;
  phaseBegin();
  phaseEnd("wifi_before");
  xTaskCreate(wifiWatcher, "watch", 4096, nullptr, 24, nullptr);
  delay(600);
  printf("@@ calling WiFi.mode(WIFI_STA), the same first call BorgesLink makes\n");
  fflush(stdout);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  const bool ok = WiFi.mode(WIFI_STA);
  printf("@@ WiFi.mode -> %d\n", ok);
  phaseEnd("wifi_after_mode", true);
  printTasks();
  fflush(stdout);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  readPlan();
  printf("\n@@ borges-qemu plan:");
  for (const auto& entry : gPlan) printf(" %s=%s", entry.first.c_str(), entry.second.c_str());
  printf("\n");

  gTaskStackBytes = 0;
  phaseBegin();
  phaseEnd("boot");
  printf("@@ reset=%d idf=%s heap_total=%u\n", static_cast<int>(esp_reset_reason()), esp_get_idf_version(),
         static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL)));
  printTasks();

  if (planInt("wifi", 0)) {
    runWifiProbe();
    return;
  }

  phaseBegin();
  const bool up = bringUpEthernet();
  phaseEnd("ethernet_up", true);
  if (!up) {
    printf("@@ no network\n@@ DONE\n");
    return;
  }

  phaseBegin();
  const bool mounted = LittleFS.begin(true, "/lfs", 8, "spiffs");
  printf("@@ littlefs mounted=%d total=%u\n", mounted, static_cast<unsigned>(LittleFS.totalBytes()));
  phaseEnd("littlefs_up");
  if (!mounted) {
    printf("@@ DONE\n");
    return;
  }

  gTaskStackBytes = static_cast<uint32_t>(planInt("stack", 16384));
  // Same call as SessionWorker::start: bytes, priority 1.
  if (xTaskCreate(sessionTask, "borges", gTaskStackBytes, nullptr, 1, nullptr) != pdPASS) {
    printf("@@ could not create the session task\n@@ DONE\n");
  }
}

// A session that goes quiet: every 30 s, what its stack holds that looks like a return
// address. Feed the values to addr2line. There is no gdb in this toolchain.
void dumpSessionStack() {
  TaskHandle_t task = xTaskGetHandle("borges");
  if (task == nullptr) return;
  const auto* top = *reinterpret_cast<uint32_t**>(task);
  const auto* end = reinterpret_cast<const uint32_t*>(pxTaskGetStackStart(task)) + gTaskStackBytes / 4;
  printf("@@ stuck? t=%lu state=%d free=%u stack:", static_cast<unsigned long>(millis()),
         static_cast<int>(eTaskGetState(task)), static_cast<unsigned>(ESP.getFreeHeap()));
  int printed = 0;
  for (const uint32_t* p = top; p < end && printed < 40; ++p) {
    if ((*p >= 0x42000000 && *p < 0x42800000) || (*p >= 0x40380000 && *p < 0x403E0000)) {
      printf(" %08lx", static_cast<unsigned long>(*p));
      ++printed;
    }
  }
  printf("\n");
  fflush(stdout);
}

void loop() {
  // Same priority as the session task, and it sleeps, as the firmware's activity loop does.
  delay(gSessionDone ? 1000 : 20);
  static uint32_t last = 0;
  if (!gSessionDone && gTaskStackBytes != 0 && millis() - last > 30000) {
    last = millis();
    dumpSessionStack();
  }
}
