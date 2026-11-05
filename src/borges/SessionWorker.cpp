#include "SessionWorker.h"

#include <Arduino.h>
#include <Logging.h>

namespace borges {
namespace glue {

namespace {
constexpr uint32_t kStackBytes = 16384;
constexpr int kPriority = 1;
}  // namespace

void SessionWorker::trampoline(void* self) {
  auto* worker = static_cast<SessionWorker*>(self);
  worker->fn_(worker->context_);
  worker->running_.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

bool SessionWorker::start(const Fn fn, void* context) {
  if (running()) return false;
  fn_ = fn;
  context_ = context;
  started_ = true;
  running_.store(true, std::memory_order_release);
  TaskHandle_t handle = nullptr;
  if (xTaskCreate(&SessionWorker::trampoline, "borges", kStackBytes, this, kPriority, &handle) != pdPASS) {
    LOG_ERR("BORGES", "OOM: session task (%u bytes, free=%u)", static_cast<unsigned>(kStackBytes), ESP.getFreeHeap());
    running_.store(false, std::memory_order_release);
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
