#include "BorgesStack.h"

#include <AppVersion.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstdlib>

#include "WifiCredentialStore.h"

namespace borges {
namespace glue {

namespace {
constexpr const char* kServerHost = "borges.runadev.com";

EngineConfig engineConfig(const char* clientVersion) {
  EngineConfig config;
  config.firmwareVersion = AppVersion::version();
  config.clientVersion = clientVersion;
  return config;
}

// Runs before any member that needs it. Member order guarantees it: both arrays are
// declared first, and this is called from the init list of the first object after them.
const uint8_t* loadIdentity(uint8_t* mac, char* clientVersion, const size_t cap, char* externalId) {
  net::readChipMac(mac);
  snprintf(clientVersion, cap, "crossink-borges/%s", AppVersion::version());
  makeExternalId(mac, externalId);
  return mac;
}
}  // namespace

bool SessionCancel::cancelled() {
  if (requested_.load(std::memory_order_relaxed)) return true;
  const uint64_t deadline = deadlineMs_.load(std::memory_order_relaxed);
  return deadline != 0 && clock_.monotonicMs() >= deadline;
}

void SessionCancel::arm(const uint32_t budgetMs) {
  requested_.store(false, std::memory_order_relaxed);
  deadlineMs_.store(budgetMs ? clock_.monotonicMs() + budgetMs : 0, std::memory_order_relaxed);
}

void SessionCancel::moveDeadline(const uint32_t budgetMs) {
  deadlineMs_.store(clock_.monotonicMs() + budgetMs, std::memory_order_relaxed);
}

void SessionCancel::moveDeadlineSinceBoot(const uint32_t budgetMs) {
  deadlineMs_.store(budgetMs, std::memory_order_relaxed);
}

uint32_t SessionCancel::remainingMs() {
  const uint64_t deadline = deadlineMs_.load(std::memory_order_relaxed);
  if (deadline == 0) return UINT32_MAX;
  const uint64_t now = clock_.monotonicMs();
  return now >= deadline ? 0 : static_cast<uint32_t>(deadline - now);
}

BorgesStack::BorgesStack()
    : box(random),
      store(fs, loadIdentity(mac, clientVersion, sizeof(clientVersion), externalId), sizeof(mac), box),
      outbox(fs, random, kOutboxDir),
      dns(fs),
      clock(dns, kServerHost),
      cancel(clock),
      http(fs, dns, &clock, clientVersion),
      link(fs),
      engine(store, outbox, fs, http, clock, random, cancel, engineConfig(clientVersion)),
      glueStore(fs) {}

bool BorgesStack::openStore() {
  if (!store.begin()) {
    LOG_ERR("BORGES", "Store could not be read");
    return false;
  }
  clock.begin(store.state().anchorEpoch);
#ifdef SIMULATOR
  // Scripted runs talk to a local fake. Compiled out of every device build.
  if (const char* url = getenv("BORGES_SIM_BASE_URL")) {
    snprintf(store.creds().baseUrl, sizeof(store.creds().baseUrl), "%s", url);
  }
#endif
  glueStore.load(glue);
  return true;
}

bool BorgesStack::openEngine() {
  if (engine.begin()) return true;
  LOG_ERR("BORGES", "Engine could not start (free=%u)", ESP.getFreeHeap());
  return false;
}

net::LinkResult BorgesStack::bringUp(const uint32_t linkBudgetMs) {
#ifdef SIMULATOR
  // A session stuck where nothing can interrupt it, to prove no screen waits on one.
  if (getenv("BORGES_SIM_HANG_WORKER")) {
    for (;;) delay(1000);
  }
#endif
  const net::LinkResult result = link.connected() ? net::LinkResult::Connected : link.connect(linkBudgetMs, cancel);
  if (result != net::LinkResult::Connected) {
    // The engine never sees this one, and the Borges screen should still say why.
    if (result != net::LinkResult::Cancelled) {
      Diagnostics& diag = store.state().diag;
      diag.clear();
      diag.atEpoch = clock.wallNow().epochSec;
      snprintf(diag.code, sizeof(diag.code), "%s", net::linkResultCode(result));
      store.saveState();
    }
    return result;
  }
  // A wrong clock is reported by TLS as tls_clock. Nothing to gain by stopping here.
  const uint32_t remaining = cancel.remainingMs();
  clock.ensure(remaining < 4000 ? remaining : 4000, cancel);
  return result;
}

void BorgesStack::bringDown() {
  http.disconnect();
  link.disconnect();
}

bool hasSavedWifi() {
#ifdef SIMULATOR
  // BORGES_SIM_NO_SAVED_WIFI: as a card with no network yet, for the first-time picker.
  if (getenv("BORGES_SIM_NO_SAVED_WIFI")) return false;
  if (getenv("BORGES_SIM_BASE_URL")) return true;
#endif
  return WIFI_STORE.getCredentialCount() > 0;
}

std::unique_ptr<BorgesStack> makeStack() {
  LOG_DBG("BORGES", "Stack: %u bytes, free=%u maxAlloc=%u", static_cast<unsigned>(sizeof(BorgesStack)),
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  auto stack = makeUniqueNoThrow<BorgesStack>();
  if (!stack) LOG_ERR("BORGES", "OOM: stack (%u bytes)", static_cast<unsigned>(sizeof(BorgesStack)));
  return stack;
}

}  // namespace glue
}  // namespace borges
