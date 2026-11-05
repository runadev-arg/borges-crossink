#pragma once

// Portable SHA-256 and a secret box built on it, for what has no wolfSSL: the host tests,
// borges-cli and the simulator. Header only, so the device image pays for it only if
// something on the device includes it, and nothing does (src/borges/net/BorgesCrypto.cpp
// goes through wolfSSL, which TLS already links).

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Ports.h"

namespace borges {

class SoftSha256 : public ISha256 {
 public:
  void begin() override {
    static constexpr uint32_t kInit[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(state_, kInit, sizeof(state_));
    total_ = 0;
    used_ = 0;
  }
  void update(const void* data, size_t len) override {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    total_ += len;
    while (len) {
      const size_t take = len < 64 - used_ ? len : 64 - used_;
      memcpy(block_ + used_, p, take);
      used_ += take;
      p += take;
      len -= take;
      if (used_ == 64) {
        transform();
        used_ = 0;
      }
    }
  }
  void finish(uint8_t digest[32]) override {
    const uint64_t bits = total_ * 8;
    block_[used_++] = 0x80;
    if (used_ > 56) {
      memset(block_ + used_, 0, 64 - used_);
      transform();
      used_ = 0;
    }
    memset(block_ + used_, 0, 56 - used_);
    for (int i = 0; i < 8; ++i) block_[56 + i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    transform();
    for (int i = 0; i < 8; ++i) {
      for (int j = 0; j < 4; ++j) digest[4 * i + j] = static_cast<uint8_t>(state_[i] >> (24 - 8 * j));
    }
  }

 private:
  static uint32_t rotr(uint32_t x, int n) { return x >> n | x << (32 - n); }
  void transform() {
    static constexpr uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = static_cast<uint32_t>(block_[4 * i]) << 24 | static_cast<uint32_t>(block_[4 * i + 1]) << 16 |
             static_cast<uint32_t>(block_[4 * i + 2]) << 8 | block_[4 * i + 3];
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t v[8];
    memcpy(v, state_, sizeof(v));
    for (int i = 0; i < 64; ++i) {
      const uint32_t s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
      const uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
      const uint32_t t1 = v[7] + s1 + ch + k[i] + w[i];
      const uint32_t s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
      const uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
      const uint32_t t2 = s0 + maj;
      memmove(v + 1, v, 7 * sizeof(uint32_t));
      v[4] += t1;
      v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; ++i) state_[i] += v[i];
  }
  uint32_t state_[8];
  uint64_t total_ = 0;
  size_t used_ = 0;
  uint8_t block_[64];
};

// Authenticated, and that is the part the tests rely on: the wrong key or a flipped byte
// fails open(). Layout as on the device: nonce(12) | ciphertext | tag(16).
class SoftSecretBox : public ISecretBox {
 public:
  SoftSecretBox(const uint8_t key[32], IRandom& random) : random_(random) { memcpy(key_, key, 32); }

  bool seal(const uint8_t* plain, size_t len, uint8_t* out) override {
    uint8_t nonce[12];
    random_.fill(nonce, sizeof(nonce));
    memcpy(out, nonce, sizeof(nonce));
    crypt(nonce, plain, len, out + sizeof(nonce));
    tag(nonce, out + sizeof(nonce), len, out + sizeof(nonce) + len);
    return true;
  }

  bool open(const uint8_t* sealed, size_t len, uint8_t* out) override {
    if (len < kOverhead) return false;
    const size_t body = len - kOverhead;
    uint8_t expect[16];
    tag(sealed, sealed + 12, body, expect);
    uint8_t diff = 0;
    for (size_t i = 0; i < sizeof(expect); ++i) diff |= expect[i] ^ sealed[12 + body + i];
    if (diff) return false;
    crypt(sealed, sealed + 12, body, out);
    return true;
  }

 private:
  void crypt(const uint8_t nonce[12], const uint8_t* in, size_t len, uint8_t* out) {
    uint8_t stream[32];
    for (size_t i = 0; i < len; ++i) {
      if (i % 32 == 0) {
        const uint32_t counter = static_cast<uint32_t>(i / 32);
        SoftSha256 sha;
        sha.begin();
        sha.update(key_, 32);
        sha.update(nonce, 12);
        sha.update(&counter, sizeof(counter));
        sha.finish(stream);
      }
      out[i] = in[i] ^ stream[i % 32];
    }
  }
  void tag(const uint8_t nonce[12], const uint8_t* cipher, size_t len, uint8_t out[16]) {
    uint8_t digest[32];
    SoftSha256 sha;
    sha.begin();
    sha.update(key_, 32);
    sha.update("tag", 3);
    sha.update(nonce, 12);
    sha.update(cipher, len);
    sha.finish(digest);
    memcpy(out, digest, 16);
  }
  uint8_t key_[32];
  IRandom& random_;
};

}  // namespace borges
