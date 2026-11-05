#pragma once

// The two cryptographic ports, over what the firmware already links for TLS: wolfSSL's
// SHA-256 and AES-GCM. A small portable SHA-256 would add about 1.3 KB of flash for
// something that is already there. The simulator has no wolfSSL and uses SoftCrypto.h.

#include <cstddef>
#include <cstdint>

#include "Ports.h"

#if defined(SIMULATOR) || defined(BORGES_NET_PROBE)
#include "SoftCrypto.h"
#else
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/sha256.h>
#endif

namespace borges {
namespace net {

#if defined(SIMULATOR) || defined(BORGES_NET_PROBE)

using DeviceSha256 = SoftSha256;

// Not a secret: a simulator's "chip" is a file on the host.
class NvsSecretBox : public ISecretBox {
 public:
  explicit NvsSecretBox(IRandom& random);
  bool seal(const uint8_t* plain, size_t len, uint8_t* out) override { return box_.seal(plain, len, out); }
  bool open(const uint8_t* sealed, size_t len, uint8_t* out) override { return box_.open(sealed, len, out); }

 private:
  SoftSecretBox box_;
};

#else

class DeviceSha256 : public ISha256 {
 public:
  void begin() override;
  void update(const void* data, size_t len) override;
  void finish(uint8_t digest[32]) override;

 private:
  wc_Sha256 sha_;
};

// AES-256-GCM with a random per-device key that lives in NVS, on the chip, and so is not on
// the card a token could be read from. The key is created the first time it is needed and
// read for each operation: nothing holds it between them.
class NvsSecretBox : public ISecretBox {
 public:
  explicit NvsSecretBox(IRandom& random) : random_(random) {}
  bool seal(const uint8_t* plain, size_t len, uint8_t* out) override;
  bool open(const uint8_t* sealed, size_t len, uint8_t* out) override;

 private:
  IRandom& random_;
};

#endif

}  // namespace net
}  // namespace borges
