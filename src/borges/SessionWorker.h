#pragma once

// Runs one blocking session off the main task, so the main loop stays free to read the
// buttons and enforce a deadline even during a TLS handshake.
//
// Stack on the heap: 16 KB. One verified TLS request measured 12.7 KB on the x86 probe, and
// RISC-V frames differ. Exists only for the session and only in network boots.

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>

namespace borges {
namespace glue {

class SessionWorker {
 public:
  using Fn = void (*)(void*);

  // False when the task could not be created. Nothing ran.
  bool start(Fn fn, void* context);
  bool running() const { return running_.load(std::memory_order_acquire); }
  // The session ended by itself since the last start().
  bool finished() const { return started_ && !running(); }
  void reset() { started_ = false; }
  // Waits up to waitMs for a session already told to cancel. False: it is still inside
  // one uninterruptible step. The caller then must not free what the session uses, and
  // must leave through a reboot or sleep, which is how every network boot ends anyway.
  bool waitForExit(uint32_t waitMs) const;

 private:
  static void trampoline(void* self);
  Fn fn_ = nullptr;
  void* context_ = nullptr;
  bool started_ = false;
  std::atomic<bool> running_{false};
};

}  // namespace glue
}  // namespace borges
