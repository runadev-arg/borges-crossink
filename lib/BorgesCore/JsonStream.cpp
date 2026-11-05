#include "JsonStream.h"

#include <cstring>

#include "Codec.h"

namespace borges {

void JsonScanner::seekTo(uint32_t offset) {
  base_ = offset;
  pos_ = 0;
  len_ = 0;
  first_ = false;
  seekPending_ = true;
}

bool JsonScanner::fail() {
  failed_ = true;
  return false;
}

bool JsonScanner::fill() {
  if (failed_) return false;
  base_ += pos_;
  pos_ = 0;
  len_ = 0;
  if (seekPending_) {
    if (!source_.seek(base_)) return fail();
    seekPending_ = false;
  }
  const int32_t n = source_.read(window_, sizeof(window_));
  if (n < 0) return fail();
  if (n == 0) return false;
  len_ = static_cast<uint16_t>(n);
  return true;
}

int JsonScanner::peekc() {
  if (pos_ >= len_ && !fill()) return -1;
  return window_[pos_];
}

int JsonScanner::getc() {
  const int c = peekc();
  if (c >= 0) ++pos_;
  return c;
}

int JsonScanner::skipWs() {
  for (;;) {
    const int c = peekc();
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') return c;
    ++pos_;
  }
}

JsonType JsonScanner::peek() {
  const int c = skipWs();
  switch (c) {
    case '{':
      return JsonType::Object;
    case '[':
      return JsonType::Array;
    case '"':
      return JsonType::String;
    case 't':
    case 'f':
      return JsonType::Bool;
    case 'n':
      return JsonType::Null;
    default:
      if (c == '-' || (c >= '0' && c <= '9')) return JsonType::Number;
      return JsonType::Invalid;
  }
}

bool JsonScanner::enterObject() {
  if (peek() != JsonType::Object) return false;
  ++pos_;
  first_ = true;
  return true;
}

bool JsonScanner::enterArray() {
  if (peek() != JsonType::Array) return false;
  ++pos_;
  first_ = true;
  return true;
}

bool JsonScanner::nextMember(char* key, size_t cap) {
  if (failed_) return false;
  int c = skipWs();
  if (c < 0) return fail();
  if (c == '}') {
    ++pos_;
    first_ = false;
    return false;
  }
  if (first_) {
    first_ = false;
  } else {
    if (c != ',') return fail();
    ++pos_;
    c = skipWs();
  }
  if (c != '"') return fail();
  ++pos_;
  if (!scanString(key, cap, nullptr)) return fail();
  if (skipWs() != ':') return fail();
  ++pos_;
  return true;
}

bool JsonScanner::nextElement() {
  if (failed_) return false;
  int c = skipWs();
  if (c < 0) return fail();
  if (c == ']') {
    ++pos_;
    first_ = false;
    return false;
  }
  if (first_) {
    first_ = false;
    return true;
  }
  if (c != ',') return fail();
  ++pos_;
  c = skipWs();
  if (c < 0 || c == ']') return fail();
  return true;
}

namespace {
int hexDigit(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Whatever a response holds ends up on the panel or on the card, so a string is clean
// before anyone sees it: no control characters but line breaks and tabs.
uint32_t displaySafe(uint32_t cp) {
  if (cp == '\n' || cp == '\t') return cp;
  return cp < 0x20 || cp == 0x7F ? ' ' : cp;
}

// A UTF-8 lead byte's sequence length (0 if it cannot start one) and the range its first
// continuation byte must fall in, which is what rules out overlong forms, surrogates and
// anything above U+10FFFF.
uint8_t utf8Lead(int c, int& low, int& high) {
  low = 0x80;
  high = 0xBF;
  if (c >= 0xC2 && c <= 0xDF) return 2;
  if (c >= 0xE0 && c <= 0xEF) {
    if (c == 0xE0) low = 0xA0;
    if (c == 0xED) high = 0x9F;
    return 3;
  }
  if (c >= 0xF0 && c <= 0xF4) {
    if (c == 0xF0) low = 0x90;
    if (c == 0xF4) high = 0x8F;
    return 4;
  }
  return 0;
}

size_t encodeUtf8(uint32_t cp, char* out) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (cp >> 18));
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}
}  // namespace

// Called after the opening quote. Consumes through the closing quote. out may be
// null: the string is still validated. What comes out is valid UTF-8 with no control
// characters: bad bytes become U+FFFD. Truncation never splits a sequence.
bool JsonScanner::scanString(char* out, size_t cap, bool* truncated) {
  size_t n = 0;
  bool cut = false;
  const auto emit = [&](const char* bytes, size_t count) {
    if (!out || cut) return;
    if (n + count + 1 > cap) {
      cut = true;
      return;
    }
    memcpy(out + n, bytes, count);
    n += count;
  };
  for (;;) {
    const int c = getc();
    if (c < 0) return false;
    if (c == '"') break;
    if (c < 0x20) return false;
    if (c != '\\') {
      // A byte past ASCII can never be a quote or a backslash, so with nowhere to put it
      // there is nothing to check.
      if (!out || cut) continue;
      if (c < 0x80) {
        const char byte = c == 0x7F ? ' ' : static_cast<char>(c);
        emit(&byte, 1);
        continue;
      }
      char sequence[4] = {static_cast<char>(c)};
      int low = 0;
      int high = 0;
      const uint8_t length = utf8Lead(c, low, high);
      uint8_t have = 1;
      while (have < length) {
        const int next = peekc();
        if (next < low || next > high) break;
        sequence[have++] = static_cast<char>(next);
        ++pos_;
        low = 0x80;
        high = 0xBF;
      }
      if (have == length && length > 0) {
        emit(sequence, length);
      } else {
        emit("\xEF\xBF\xBD", 3);
      }
      continue;
    }
    const int e = getc();
    char plain = 0;
    switch (e) {
      case '"':
      case '\\':
      case '/':
        plain = static_cast<char>(e);
        break;
      case 'n':
        plain = '\n';
        break;
      case 't':
        plain = '\t';
        break;
      case 'b':
      case 'f':
      case 'r':
        plain = ' ';
        break;
      case 'u':
        break;
      default:
        return false;
    }
    if (plain) {
      emit(&plain, 1);
      continue;
    }
    uint32_t cp = 0;
    for (int i = 0; i < 4; ++i) {
      const int d = hexDigit(getc());
      if (d < 0) return false;
      cp = cp << 4 | static_cast<uint32_t>(d);
    }
    if (cp >= 0xD800 && cp <= 0xDBFF && peekc() == '\\') {
      // A high surrogate only combines with an immediately following \uDC00-DFFF.
      ++pos_;
      if (getc() != 'u') return false;
      uint32_t low = 0;
      for (int i = 0; i < 4; ++i) {
        const int d = hexDigit(getc());
        if (d < 0) return false;
        low = low << 4 | static_cast<uint32_t>(d);
      }
      cp = (low >= 0xDC00 && low <= 0xDFFF) ? 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00) : 0xFFFD;
    } else if ((cp >= 0xD800 && cp <= 0xDFFF) || cp == 0) {
      cp = 0xFFFD;  // A lone surrogate, or a NUL that would end the C string early.
    }
    char encoded[4];
    const size_t count = encodeUtf8(displaySafe(cp), encoded);
    emit(encoded, count);
  }
  if (out && cap > 0) out[n] = '\0';
  if (truncated) *truncated = cut;
  return true;
}

// Strict JSON number grammar. out receives the token when it fits.
bool JsonScanner::scanNumber(char* out, size_t cap, size_t* used) {
  size_t n = 0;
  const auto take = [&]() {
    const int c = getc();
    if (out && n + 1 < cap) out[n] = static_cast<char>(c);
    ++n;
  };
  if (peekc() == '-') take();
  int c = peekc();
  if (c == '0') {
    take();
  } else if (c >= '1' && c <= '9') {
    while ((c = peekc()) >= '0' && c <= '9') take();
  } else {
    return false;
  }
  if (peekc() == '.') {
    take();
    c = peekc();
    if (c < '0' || c > '9') return false;
    while ((c = peekc()) >= '0' && c <= '9') take();
  }
  c = peekc();
  if (c == 'e' || c == 'E') {
    take();
    c = peekc();
    if (c == '+' || c == '-') take();
    c = peekc();
    if (c < '0' || c > '9') return false;
    while ((c = peekc()) >= '0' && c <= '9') take();
  }
  if (out && cap > 0) out[n < cap ? n : cap - 1] = '\0';
  if (used) *used = n;
  return true;
}

bool JsonScanner::scanLiteral(const char* rest) {
  for (const char* p = rest; *p; ++p) {
    if (getc() != *p) return false;
  }
  return true;
}

// Values nested past the type stack are not checked for colons and commas, only
// balanced and string-aware. Such an event ends up skipped, never a failed page.
bool JsonScanner::skipDeep() {
  uint64_t depth = 1;
  while (depth) {
    const int c = getc();
    if (c < 0) return fail();
    if (c == '{' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ']') {
      --depth;
    } else if (c == '"') {
      if (!scanString(nullptr, 0, nullptr)) return fail();
    }
  }
  return true;
}

bool JsonScanner::skipValue() {
  if (failed_) return false;
  // Bit i of stack: container at depth i+1 is an object.
  uint64_t stack = 0;
  uint32_t depth = 0;
  enum class Expect : uint8_t { Value, ValueOrEnd, KeyOrEnd, Key, CommaOrEnd };
  Expect expect = Expect::Value;
  for (;;) {
    int c = skipWs();
    if (c < 0) return fail();
    const bool inObject = depth > 0 && ((stack >> (depth - 1)) & 1u);
    switch (expect) {
      case Expect::KeyOrEnd:
      case Expect::Key:
        if (c == '}' && expect == Expect::KeyOrEnd) {
          ++pos_;
          --depth;
          expect = Expect::CommaOrEnd;
          break;
        }
        if (c != '"') return fail();
        ++pos_;
        if (!scanString(nullptr, 0, nullptr)) return fail();
        if (skipWs() != ':') return fail();
        ++pos_;
        expect = Expect::Value;
        break;
      case Expect::CommaOrEnd:
        if (c == ',') {
          ++pos_;
          expect = inObject ? Expect::Key : Expect::Value;
        } else if ((c == '}' && inObject) || (c == ']' && !inObject)) {
          ++pos_;
          --depth;
        } else {
          return fail();
        }
        break;
      case Expect::ValueOrEnd:
        if (c == ']') {
          ++pos_;
          --depth;
          expect = Expect::CommaOrEnd;
          break;
        }
        [[fallthrough]];
      case Expect::Value:
        if (c == '{' || c == '[') {
          ++pos_;
          if (depth >= 64) {
            if (!skipDeep()) return false;
            expect = Expect::CommaOrEnd;
            break;
          }
          if (c == '{') {
            stack |= (1ULL << depth);
          } else {
            stack &= ~(1ULL << depth);
          }
          ++depth;
          expect = c == '{' ? Expect::KeyOrEnd : Expect::ValueOrEnd;
          break;
        }
        if (c == '"') {
          ++pos_;
          if (!scanString(nullptr, 0, nullptr)) return fail();
        } else if (c == 't') {
          if (!scanLiteral("true")) return fail();
        } else if (c == 'f') {
          if (!scanLiteral("false")) return fail();
        } else if (c == 'n') {
          if (!scanLiteral("null")) return fail();
        } else if (!scanNumber(nullptr, 0, nullptr)) {
          return fail();
        }
        expect = Expect::CommaOrEnd;
        break;
    }
    if (depth == 0 && expect == Expect::CommaOrEnd) {
      first_ = false;
      return true;
    }
  }
}

bool JsonScanner::readString(char* out, size_t cap, bool* truncated) {
  if (truncated) *truncated = false;
  if (cap > 0) out[0] = '\0';
  if (peek() != JsonType::String) {
    skipValue();
    return false;
  }
  ++pos_;
  if (!scanString(out, cap, truncated)) return fail();
  first_ = false;
  return true;
}

bool JsonScanner::readSequence(uint64_t& out) {
  char text[24];
  bool truncated = false;
  if (!readString(text, sizeof(text), &truncated) || truncated) return false;
  return parseSequence(text, out);
}

bool JsonScanner::readInt(int64_t& out) {
  if (peek() != JsonType::Number) {
    skipValue();
    return false;
  }
  char token[24];
  size_t used = 0;
  if (!scanNumber(token, sizeof(token), &used)) return fail();
  first_ = false;
  if (used >= sizeof(token)) return false;
  const char* p = token;
  const bool negative = *p == '-';
  if (negative) ++p;
  uint64_t value = 0;
  for (; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
    const uint64_t digit = static_cast<uint64_t>(*p - '0');
    if (value > (0x7FFFFFFFFFFFFFFFULL - digit) / 10) return false;
    value = value * 10 + digit;
  }
  out = negative ? -static_cast<int64_t>(value) : static_cast<int64_t>(value);
  return true;
}

// Digit-by-digit so the result is exact and needs no float parser. Anything outside
// 0..100 is "not usable", the same range the server enforces.
bool JsonScanner::readPercentMicro(uint32_t& out) {
  if (peek() != JsonType::Number) {
    skipValue();
    return false;
  }
  char token[48];
  size_t used = 0;
  if (!scanNumber(token, sizeof(token), &used)) return fail();
  first_ = false;
  if (used >= sizeof(token) || token[0] == '-') {
    // "-0" style values and absurdly long tokens are not worth a special case.
    return false;
  }
  const char* p = token;
  // Collect significant digits and the decimal exponent.
  uint64_t mantissa = 0;
  int scale = 0;
  bool saturated = false;
  for (; *p >= '0' && *p <= '9'; ++p) {
    if (mantissa < 100000000000000ULL) {
      mantissa = mantissa * 10 + static_cast<uint64_t>(*p - '0');
    } else {
      saturated = true;
    }
  }
  if (saturated) return false;
  if (*p == '.') {
    ++p;
    for (; *p >= '0' && *p <= '9'; ++p) {
      if (mantissa < 100000000000000ULL) {
        mantissa = mantissa * 10 + static_cast<uint64_t>(*p - '0');
        --scale;
      }
    }
  }
  if (*p == 'e' || *p == 'E') {
    ++p;
    const bool negative = *p == '-';
    if (*p == '+' || *p == '-') ++p;
    int exponent = 0;
    for (; *p >= '0' && *p <= '9'; ++p) {
      if (exponent < 10000) exponent = exponent * 10 + (*p - '0');
    }
    scale += negative ? -exponent : exponent;
  }
  scale += 6;  // Millionths.
  while (scale > 0 && mantissa) {
    if (mantissa > 100000000ULL * 10) return false;
    mantissa *= 10;
    --scale;
  }
  // The last digit dropped is the one that rounds: a digit dropped earlier must not
  // survive the mantissa running out before the scale does.
  bool roundUp = false;
  for (; scale < 0; ++scale) {
    roundUp = mantissa % 10 >= 5;
    mantissa /= 10;
    if (mantissa == 0 && scale < -1) {
      roundUp = false;
      break;
    }
  }
  if (roundUp) ++mantissa;
  if (mantissa > 100000000ULL) return false;
  out = static_cast<uint32_t>(mantissa);
  return true;
}

bool JsonScanner::readBool(bool& out) {
  if (peek() != JsonType::Bool) {
    skipValue();
    return false;
  }
  const bool value = peekc() == 't';
  if (!scanLiteral(value ? "true" : "false")) return fail();
  first_ = false;
  out = value;
  return true;
}

bool JsonScanner::atEnd() {
  if (failed_) return false;
  return skipWs() < 0 && !failed_;
}

}  // namespace borges
