#pragma once

// IFs over HalStorage (the real SD on the device, SimFs in the simulator). Holds four
// open files at most, per the contract in Ports.h. Takes no task affinity of its own:
// HalStorage serializes access with its mutex. Keep one instance per task.

#include <HalStorage.h>

#include "Ports.h"

namespace borges {
namespace net {

class BorgesFsHal : public IFs {
 public:
  BorgesFsHal() = default;
  ~BorgesFsHal() override;

  Handle open(const char* path, Mode mode) override;
  int32_t read(Handle h, void* dst, size_t len) override;
  int32_t write(Handle h, const void* src, size_t len) override;
  bool seek(Handle h, uint32_t pos) override;
  int64_t size(Handle h) override;
  bool flush(Handle h) override;  // HalFile::sync(): its result, unlike flush(), is checked.
  void close(Handle h) override;
  bool exists(const char* path) override;
  bool remove(const char* path) override;
  bool rename(const char* from, const char* to) override;  // SdFat refuses an existing target.
  bool mkdir(const char* path) override;

 private:
  static constexpr int kSlots = 4;
  HalFile* file(Handle h);
  HalFile files_[kSlots];
};

}  // namespace net
}  // namespace borges
