#include "BorgesFsHal.h"

#include <utility>

namespace borges {
namespace net {

BorgesFsHal::~BorgesFsHal() {
  for (HalFile& f : files_) {
    if (f.isOpen()) f.close();
  }
}

HalFile* BorgesFsHal::file(const Handle h) {
  if (h < 0 || h >= kSlots || !files_[h].isOpen()) return nullptr;
  return &files_[h];
}

IFs::Handle BorgesFsHal::open(const char* path, const Mode mode) {
  Handle slot = kInvalid;
  for (Handle i = 0; i < kSlots; ++i) {
    if (!files_[i].isOpen()) {
      slot = i;
      break;
    }
  }
  if (slot == kInvalid) return kInvalid;

  oflag_t flags = O_RDONLY;
  if (mode == Mode::WriteTrunc) flags = O_RDWR | O_CREAT | O_TRUNC;
  if (mode == Mode::Append) flags = O_RDWR | O_CREAT | O_APPEND;
  HalFile opened = Storage.open(path, flags);
  if (!opened.isOpen()) return kInvalid;
  files_[slot] = std::move(opened);
  return slot;
}

int32_t BorgesFsHal::read(const Handle h, void* dst, const size_t len) {
  HalFile* f = file(h);
  if (f == nullptr) return -1;
  return f->read(dst, len);
}

int32_t BorgesFsHal::write(const Handle h, const void* src, const size_t len) {
  HalFile* f = file(h);
  if (f == nullptr) return -1;
  const size_t written = f->write(src, len);
  // SdFat reports a failed write as 0, or as a short count on a full card. The core
  // compares the count with what it asked for, so a short one stays visible.
  if (written == 0 && len != 0) return -1;
  return static_cast<int32_t>(written);
}

bool BorgesFsHal::seek(const Handle h, const uint32_t pos) {
  HalFile* f = file(h);
  return f != nullptr && f->seek(static_cast<size_t>(pos));
}

int64_t BorgesFsHal::size(const Handle h) {
  HalFile* f = file(h);
  if (f == nullptr) return -1;
  return static_cast<int64_t>(f->fileSize64());
}

bool BorgesFsHal::flush(const Handle h) {
  HalFile* f = file(h);
  return f != nullptr && f->sync();
}

void BorgesFsHal::close(const Handle h) {
  HalFile* f = file(h);
  if (f != nullptr) f->close();
}

bool BorgesFsHal::exists(const char* path) { return Storage.exists(path); }

bool BorgesFsHal::remove(const char* path) { return !Storage.exists(path) || Storage.remove(path); }

bool BorgesFsHal::rename(const char* from, const char* to) { return Storage.rename(from, to); }

bool BorgesFsHal::mkdir(const char* path) { return Storage.exists(path) || Storage.mkdir(path, true); }

}  // namespace net
}  // namespace borges
