#pragma once

// Every port and core object one Borges use needs, in a single heap block that exists
// only while it is used. Nothing here is resident: a reader that is just reading holds
// none of it (design rule: no always-on memory while reading).
//
// Heap: sizeof(BorgesStack) plus the engine's scratch and outbox index (10 KB at most)
// once begin() ran. A single allocation, because every member has the same lifetime and
// separate ones would only leave more holes.

#include <atomic>
#include <memory>

#include "Catalog.h"
#include "Outbox.h"
#include "PairingFlow.h"
#include "Store.h"
#include "SyncEngine.h"
#include "borges/logic/GlueState.h"
#include "borges/net/BorgesClock.h"
#include "borges/net/BorgesDevice.h"
#include "borges/net/BorgesDns.h"
#include "borges/net/BorgesFsHal.h"
#include "borges/net/BorgesHttp.h"
#include "borges/net/BorgesLink.h"

namespace borges {
namespace glue {

// One flag for the whole stack: a button or a deadline. Written by the main task, read
// by the session task.
class SessionCancel : public ICancel {
 public:
  explicit SessionCancel(IClock& clock) : clock_(clock) {}
  bool cancelled() override;
  void request() { requested_.store(true, std::memory_order_relaxed); }
  // Clears the request and starts a new deadline. 0 means no deadline.
  void arm(uint32_t budgetMs);
  bool requested() const { return requested_.load(std::memory_order_relaxed); }
  uint32_t remainingMs();

 private:
  IClock& clock_;
  std::atomic<bool> requested_{false};
  std::atomic<uint64_t> deadlineMs_{0};
};

struct BorgesStack {
  BorgesStack();

  // False on a hard SD error. Reads the store only: no engine scratch yet.
  bool openStore();
  // Scratch and the outbox, for anything that goes through the engine.
  bool openEngine();

  char clientVersion[48] = {0};
  char externalId[20] = {0};
  uint8_t mac[6] = {0};

  net::BorgesFsHal fs;
  net::EspRandom random;
  Store store;
  Outbox outbox;
  net::BorgesDns dns;
  net::BorgesClock clock;
  SessionCancel cancel;
  net::BorgesHttp http;
  net::BorgesLink link;
  SyncEngine engine;
  GlueStore glueStore;
  GlueState glue;

  // True when the radio is up and the clock has been given its chance. Any failure has
  // already turned the radio off.
  net::LinkResult bringUp(uint32_t linkBudgetMs);
  // Radio off, connection closed. Every path out of a session comes through here.
  void bringDown();
};

// A network this device can try without asking the reader anything.
bool hasSavedWifi();

// nullptr when the heap cannot afford it: the caller goes on without Borges.
std::unique_ptr<BorgesStack> makeStack();

}  // namespace glue
}  // namespace borges
