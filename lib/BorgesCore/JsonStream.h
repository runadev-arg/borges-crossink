#pragma once

// Pull parser over a seekable byte source. Responses are spooled to the SD card
// first, then walked here with a fixed 192-byte window: memory use does not depend
// on the size or the shape of what the server sent, and nothing recurses.
//
// read*() on a value of an unexpected type consumes it and returns false without
// failing the document, so a hostile field cannot poison the element around it.
// Only malformed JSON or a truncated stream sets failed(), and that is sticky.

#include <cstddef>
#include <cstdint>

#include "Ports.h"

namespace borges {

class IByteSource {
 public:
  virtual ~IByteSource() = default;
  virtual int32_t read(uint8_t* dst, size_t len) = 0;  // 0 at EOF, <0 on error.
  virtual bool seek(uint32_t pos) = 0;
};

class FileByteSource : public IByteSource {
 public:
  explicit FileByteSource(File& file) : file_(file) {}
  int32_t read(uint8_t* dst, size_t len) override { return file_.read(dst, len); }
  bool seek(uint32_t pos) override { return file_.seek(pos); }

 private:
  File& file_;
};

enum class JsonType : uint8_t { Invalid, Object, Array, String, Number, Bool, Null };

class JsonScanner {
 public:
  explicit JsonScanner(IByteSource& source) : source_(source) {}

  // Repositions at a value recorded earlier with offset().
  void seekTo(uint32_t offset);
  uint32_t offset() const { return base_ + pos_; }  // Offset of the next unread byte.
  bool failed() const { return failed_; }

  JsonType peek();  // Skips whitespace; does not consume the value.

  bool enterObject();                      // False (nothing consumed) if not an object.
  bool nextMember(char* key, size_t cap);  // True when positioned at a member value.
  bool enterArray();                       // False (nothing consumed) if not an array.
  bool nextElement();                      // True when positioned at an element.

  // Each of these consumes the value in every case. False means "not usable".
  bool readString(char* out, size_t cap, bool* truncated = nullptr);
  bool readSequence(uint64_t& out);      // Decimal string, per the wire rule for 64-bit integers.
  bool readInt(int64_t& out);            // JSON integer.
  bool readPercentMicro(uint32_t& out);  // JSON number within 0..100, as millionths.
  bool readBool(bool& out);
  bool skipValue();  // Validates structure. Depth only costs a counter.

  // True when only whitespace remains.
  bool atEnd();

 private:
  int getc();
  int peekc();
  int skipWs();
  bool fail();
  bool fill();
  bool scanString(char* out, size_t cap, bool* truncated);
  bool scanNumber(char* out, size_t cap, size_t* used);
  bool scanLiteral(const char* rest);
  bool skipDeep();  // Beyond the type stack: only counts brackets, string-aware.

  IByteSource& source_;
  uint32_t base_ = 0;
  uint16_t pos_ = 0;
  uint16_t len_ = 0;
  bool first_ = false;
  bool failed_ = false;
  bool seekPending_ = true;
  uint8_t window_[192];
};

}  // namespace borges
