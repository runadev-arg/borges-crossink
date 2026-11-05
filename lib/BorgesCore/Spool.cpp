#include "Spool.h"

namespace borges {

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
