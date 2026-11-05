#pragma once

// The slice of Arduino the SDK's SecureClient and SecureHttpClient touch, over Winsock.
// Only for the TLS probe: the point is that the real SDK files compile unchanged.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <strings.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

inline unsigned long millis() { return static_cast<unsigned long>(GetTickCount64()); }
inline void delay(unsigned long ms) { Sleep(ms); }

class IPAddress {
 public:
  IPAddress() = default;
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : bytes_{a, b, c, d} {}
  uint8_t operator[](int i) const { return bytes_[i]; }

 private:
  uint8_t bytes_[4] = {0, 0, 0, 0};
};

struct ProbeSerial {
  bool quiet = true;
  explicit operator bool() const { return !quiet; }
  void printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
  }
  void println(const char* text) { puts(text); }
};
inline ProbeSerial Serial;

// Free heap is whatever the scenario says it is: the gate is what is under test.
struct ProbeEsp {
  uint32_t freeHeap = 200 * 1024;
  uint32_t maxAlloc = 100 * 1024;
  uint32_t getFreeHeap() const { return freeHeap; }
  uint32_t getMaxAllocHeap() const { return maxAlloc; }
};
inline ProbeEsp ESP;
