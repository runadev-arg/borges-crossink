#include "Spool.h"

namespace borges {

void scrubFile(IFs& fs, const char* path) {
  if (!fs.exists(path)) return;
  {
    File f(fs, path, IFs::Mode::Update);
    if (f) {
      int64_t left = f.size();
      static constexpr uint8_t kZeros[64] = {0};
      while (left > 0) {
        const size_t n = left < static_cast<int64_t>(sizeof(kZeros)) ? static_cast<size_t>(left) : sizeof(kZeros);
        if (!f.writeAll(kZeros, n)) break;
        left -= static_cast<int64_t>(n);
      }
      f.flush();
    }
  }
  fs.remove(path);
}

bool Spool::begin() {
  close();
  size_ = 0;
  ioFailed_ = false;
  handle_ = fs_.open(path_, IFs::Mode::WriteTrunc);
  if (handle_ == IFs::kInvalid) {
    ioFailed_ = true;
    return false;
  }
  return true;
}

bool Spool::onBody(const uint8_t* data, size_t len) {
  if (handle_ == IFs::kInvalid) {
    ioFailed_ = true;
    return false;
  }
  if (len > kMaxBytes - size_) return false;  // Aborts the transfer: reads as a dropped connection.
  if (len && fs_.write(handle_, data, len) != static_cast<int32_t>(len)) {
    ioFailed_ = true;
    return false;
  }
  size_ += static_cast<uint32_t>(len);
  return true;
}

void Spool::close() {
  if (handle_ != IFs::kInvalid) {
    if (!fs_.flush(handle_)) ioFailed_ = true;
    fs_.close(handle_);
  }
  handle_ = IFs::kInvalid;
}

}  // namespace borges
