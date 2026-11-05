#pragma once

// Responses go to SD first (design rule 1). The cap is not a parsing limit, it is what
// keeps a broken or hostile server from filling the card: far above anything a server
// page can be (48 KiB of events). A spool does not need to survive a crash, so it is never
// flushed before parsing beyond close.

#include <cstddef>
#include <cstdint>

#include "Ports.h"

namespace borges {

// Overwrites a file on its own clusters, then removes it. Removing alone leaves the bytes
// on the card, and a response may hold a token. Best effort: nothing to do if this fails.
void scrubFile(IFs& fs, const char* path);

class Spool : public IHttpSink {
 public:
  Spool(IFs& fs, const char* path) : fs_(fs), path_(path) {}
  ~Spool() override { close(); }

  static constexpr uint32_t kMaxBytes = 4 * 1024 * 1024;

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
