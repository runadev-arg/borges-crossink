#include "BorgesCrypto.h"

#include <cstring>

#if !defined(SIMULATOR) && !defined(BORGES_NET_PROBE)
#include <Preferences.h>
#include <bootloader_random.h>
#include <esp_random.h>
#include <wolfssl/wolfcrypt/aes.h>

#include <new>
#endif

namespace borges {
namespace net {

#if defined(SIMULATOR) || defined(BORGES_NET_PROBE)

namespace {
const uint8_t kFixedKey[32] = {'b', 'o', 'r', 'g', 'e', 's', '-', 's', 'i', 'm'};
}  // namespace

NvsSecretBox::NvsSecretBox(IRandom& random) : box_(kFixedKey, random) {}

#else

namespace {

constexpr const char* kNamespace = "borges";
constexpr const char* kKeyName = "credkey";
constexpr size_t kKeyBytes = 32;
constexpr size_t kNonceBytes = 12;
constexpr size_t kTagBytes = 16;
static_assert(ISecretBox::kOverhead == kNonceBytes + kTagBytes);

// The existing key, or a new one. esp_fill_random is only pseudo-random with the radio
// off unless the bootloader's entropy source is on, and this runs with the radio off.
bool loadKey(uint8_t key[kKeyBytes]) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;
  bool ok = prefs.getBytesLength(kKeyName) == kKeyBytes && prefs.getBytes(kKeyName, key, kKeyBytes) == kKeyBytes;
  if (!ok) {
    bootloader_random_enable();
    esp_fill_random(key, kKeyBytes);
    bootloader_random_disable();
    ok = prefs.putBytes(kKeyName, key, kKeyBytes) == kKeyBytes;
  }
  prefs.end();
  return ok;
}

// Aes is large for this stack and only ever lives for one operation.
class GcmKey {
 public:
  GcmKey() : aes_(new (std::nothrow) Aes) {
    uint8_t key[kKeyBytes];
    ready_ = aes_ && loadKey(key) && wc_AesInit(aes_, nullptr, INVALID_DEVID) == 0;
    inited_ = ready_;
    if (ready_) ready_ = wc_AesGcmSetKey(aes_, key, kKeyBytes) == 0;
    memset(key, 0, sizeof(key));
  }
  ~GcmKey() {
    if (inited_) wc_AesFree(aes_);
    delete aes_;
  }
  bool ready() const { return ready_; }
  Aes* get() { return aes_; }

 private:
  Aes* aes_;
  bool ready_ = false;
  bool inited_ = false;
};

}  // namespace

bool NvsSecretBox::seal(const uint8_t* plain, const size_t len, uint8_t* out) {
  GcmKey key;
  if (!key.ready()) return false;
  random_.fill(out, kNonceBytes);
  return wc_AesGcmEncrypt(key.get(), out + kNonceBytes, plain, len, out, kNonceBytes, out + kNonceBytes + len, kTagBytes,
                          nullptr, 0) == 0;
}

bool NvsSecretBox::open(const uint8_t* sealed, const size_t len, uint8_t* out) {
  if (len < kOverhead) return false;
  GcmKey key;
  if (!key.ready()) return false;
  const size_t body = len - kOverhead;
  return wc_AesGcmDecrypt(key.get(), out, sealed + kNonceBytes, body, sealed, kNonceBytes, sealed + kNonceBytes + body,
                          kTagBytes, nullptr, 0) == 0;
}

void DeviceSha256::begin() { wc_InitSha256(&sha_); }

void DeviceSha256::update(const void* data, const size_t len) {
  wc_Sha256Update(&sha_, static_cast<const uint8_t*>(data), static_cast<word32>(len));
}

void DeviceSha256::finish(uint8_t digest[32]) { wc_Sha256Final(&sha_, digest); }

#endif

}  // namespace net
}  // namespace borges
