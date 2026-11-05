#include "SessionWorker.h"

#include <Arduino.h>
#include <Logging.h>

#include <new>

namespace borges {
namespace glue {

namespace {
constexpr uint32_t kStackBytes = 16384;
constexpr int kPriority = 1;
}  // namespace

void SessionWorker::trampoline(void* state) {
  auto* self = static_cast<State*>(state);
  self->fn(self->context);
  self->running.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

SessionWorker::~SessionWorker() {
  // Still running: left behind with the context, until the reboot or sleep that is coming.
  if (!running()) delete state_;
}

bool SessionWorker::start(const Fn fn, void* context) {
  if (running()) return false;
  if (!state_) state_ = new (std::nothrow) State;
  if (!state_) return false;
  state_->fn = fn;
  state_->context = context;
  started_ = true;
  state_->running.store(true, std::memory_order_release);
  TaskHandle_t handle = nullptr;
  if (xTaskCreate(&SessionWorker::trampoline, "borges", kStackBytes, state_, kPriority, &handle) != pdPASS) {
    LOG_ERR("BORGES", "OOM: session task (%u bytes, free=%u)", static_cast<unsigned>(kStackBytes), ESP.getFreeHeap());
    state_->running.store(false, std::memory_order_release);
    return false;
  }
  return true;
}

bool SessionWorker::waitForExit(const uint32_t waitMs) const {
  const uint32_t start = millis();
  while (running()) {
    if (millis() - start >= waitMs) return false;
    delay(20);
  }
  return true;
}

}  // namespace glue
}  // namespace borges
