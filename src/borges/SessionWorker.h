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

  ~SessionWorker();

  // False when the task could not be created. Nothing ran.
  bool start(Fn fn, void* context);
  bool running() const { return state_ && state_->running.load(std::memory_order_acquire); }
  // The session ended by itself since the last start().
  bool finished() const { return started_ && !running(); }
  void reset() { started_ = false; }
  // Waits up to waitMs for a session already told to cancel. False: it is still inside
  // one uninterruptible step (a socket read blocks for up to 10 s). The caller then must not
  // free the context, and leaks it: every network boot ends in a reboot or sleep.
  bool waitForExit(uint32_t waitMs) const;

 private:
  // On the heap: a session that outlives its activity still writes here on its way out.
  struct State {
    Fn fn = nullptr;
    void* context = nullptr;
    std::atomic<bool> running{false};
  };
  static void trampoline(void* state);
  State* state_ = nullptr;
  bool started_ = false;
};

}  // namespace glue
}  // namespace borges
