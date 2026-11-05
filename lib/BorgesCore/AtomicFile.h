#pragma once

// Two-slot CRC-checked record. A torn or lost write always falls back to the last
// good version, without relying on rename: FAT cannot rename over a file atomically.
//
//   <base>.a / <base>.b:  "BAF1" | seq u32 | len u32 | crc u32 | payload
//
// save() writes the slot that does not hold the newest valid record, then flushes.
// The commit point is the flush: until then the other slot still wins.

#include <cstddef>
#include <cstdint>

#include "Ports.h"

namespace borges {

class AtomicFile {
 public:
  static constexpr size_t kMaxPath = 96;

  AtomicFile(IFs& fs, const char* basePath);
  void setPath(const char* basePath);  // Forgets what it knew about the previous path.

  // Newest valid payload. False when no slot is valid, or it does not fit in cap.
  bool load(uint8_t* buf, size_t cap, size_t& len);
  bool save(const void* payload, size_t len);
  void removeAll();

  uint32_t sequence() const { return seq_; }

 private:
  struct Probe {
    bool valid = false;
    uint32_t seq = 0;
    uint32_t len = 0;
  };
  Probe probe(char slot);
  void slotPath(char slot, char* out) const;
  void refresh();

  IFs& fs_;
  char base_[kMaxPath];
  bool probed_ = false;
  char active_ = 0;  // Slot holding the newest valid record; 0 when there is none.
  uint32_t seq_ = 0;
};

}  // namespace borges
