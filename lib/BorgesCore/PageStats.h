#pragma once

// Time spent per page, for what the account dashboard reads: hours, heatmap, streaks and
// per-book stats all come from page_stats (highlights-kobo, sql/041). Held by the reader:
// no heap and no IO while reading, and a page turn costs a few integers. finalizeBook()
// turns the rows into page_stat.recorded events and clears them.
//
// When it fills up, neighbouring rows merge rather than get lost: what the dashboard adds up
// is time per day and per book, and that survives. A merged row stays within the 600 s the
// dashboard caps a row at, or the merge itself would lose time there.

#include <cstdint>

namespace borges {

struct PageStatRow {
  uint32_t startEpoch;
  uint16_t page;
  uint16_t seconds;
};

class PageStatBuffer {
 public:
  static constexpr uint8_t kCapacity = 96;
  static constexpr int64_t kMinSeconds = 5;    // A page flipped past was not read.
  static constexpr int64_t kMaxSeconds = 600;  // The dashboard's cap per row. A nap is not reading.

  // The reader left page after having entered it at enteredEpoch. Any position change counts.
  void onLeave(int32_t page, int64_t enteredEpoch, int64_t nowEpoch) {
    const int64_t spent = nowEpoch - enteredEpoch;
    if (enteredEpoch <= 0 || spent < kMinSeconds) return;
    if (count_ == kCapacity) compact();
    PageStatRow& row = rows_[count_++];
    row.startEpoch = static_cast<uint32_t>(enteredEpoch);
    row.page = page < 0 ? 0 : page > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(page);
    row.seconds = static_cast<uint16_t>(spent > kMaxSeconds ? kMaxSeconds : spent);
  }

  uint8_t count() const { return count_; }
  const PageStatRow& at(uint8_t index) const { return rows_[index]; }
  void clear() { count_ = 0; }
  // The first n rows made it into the queue.
  void dropFront(uint8_t n) {
    for (uint8_t i = n; i < count_; ++i) rows_[i - n] = rows_[i];
    count_ = static_cast<uint8_t>(count_ - n);
  }

 private:
  void compact() {
    uint8_t kept = 0;
    for (uint8_t i = 0; i < count_; ++i) {
      if (kept > 0 && rows_[kept - 1].seconds + rows_[i].seconds <= kMaxSeconds) {
        rows_[kept - 1].seconds = static_cast<uint16_t>(rows_[kept - 1].seconds + rows_[i].seconds);
      } else {
        rows_[kept++] = rows_[i];
      }
    }
    // Every row was already near the cap: over nine hours in this buffer. The oldest goes.
    if (kept == kCapacity) {
      dropFront(1);
      return;
    }
    count_ = kept;
  }

  PageStatRow rows_[kCapacity];
  uint8_t count_ = 0;
};

}  // namespace borges
