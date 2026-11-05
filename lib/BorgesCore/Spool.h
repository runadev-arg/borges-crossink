#pragma once

// Responses go to SD first (design rule 1). There is deliberately no size cap to
// wedge on: the session's wall-clock budget is what bounds a response. A spool does
// not need to survive a crash, so it is never flushed before parsing beyond close.

#include <cstddef>
#include <cstdint>

#include "Ports.h"

namespace borges {

class Spool : public IHttpSink {
 public:
  Spool(IFs& fs, const char* path) : fs_(fs), path_(path) {}
  ~Spool() override { close(); }

  bool begin();  // Truncates. False on IO error.
  bool onBody(const uint8_t* data, size_t len) override;
  void close();

  uint32_t size() const { return size_; }
  bool ioFailed() const { return ioFailed_; }
  const char* path() const { return path_; }

 private:
  IFs& fs_;
  const char* path_;
  IFs::Handle handle_ = IFs::kInvalid;
  uint32_t size_ = 0;
  bool ioFailed_ = false;
};

}  // namespace borges
