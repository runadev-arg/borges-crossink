// A file in memory, for feeding the real CssParser on the host.
#pragma once

#include <Print.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

class HalFile : public Print {
 public:
  HalFile() = default;
  explicit HalFile(std::string data) : data_(std::move(data)) {}
  int available() const { return static_cast<int>(data_.size() - pos_); }
  size_t position() const { return pos_; }
  size_t size() const { return data_.size(); }
  int read(void* out, size_t n) {
    const size_t take = std::min(n, data_.size() - pos_);
    memcpy(out, data_.data() + pos_, take);
    pos_ += take;
    return static_cast<int>(take);
  }
  bool seek(size_t p) {
    pos_ = std::min(p, data_.size());
    return true;
  }
  bool seekSet(size_t p) { return seek(p); }
  bool close() { return true; }
  bool isOpen() const { return true; }
  operator bool() const { return true; }
  size_t write(uint8_t) override { return 0; }
  size_t write(const uint8_t*, size_t) override { return 0; }

 private:
  std::string data_;
  size_t pos_ = 0;
};

using FsFile = HalFile;

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }
  bool openFileForRead(const char*, const std::string&, HalFile&) { return false; }
  bool openFileForWrite(const char*, const std::string&, HalFile&) { return false; }
  bool remove(const char*) { return false; }
  bool exists(const char*) const { return false; }
  bool rename(const char*, const char*) { return false; }
};

#define Storage HalStorage::getInstance()
