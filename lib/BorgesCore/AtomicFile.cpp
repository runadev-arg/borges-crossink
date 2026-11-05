#include "AtomicFile.h"

#include <cstring>

#include "Codec.h"

namespace borges {

namespace {
constexpr uint8_t kMagic[4] = {'B', 'A', 'F', '1'};
constexpr size_t kHeader = 16;
constexpr uint32_t kMaxPayload = 64 * 1024;
}  // namespace

AtomicFile::AtomicFile(IFs& fs, const char* basePath) : fs_(fs) { copyText(base_, sizeof(base_), basePath); }

void AtomicFile::setPath(const char* basePath) {
  copyText(base_, sizeof(base_), basePath);
  probed_ = false;
  active_ = 0;
  seq_ = 0;
}

void AtomicFile::slotPath(char slot, char* out) const {
  const size_t n = strlen(base_);
  memcpy(out, base_, n);
  out[n] = '.';
  out[n + 1] = slot;
  out[n + 2] = '\0';
}

AtomicFile::Probe AtomicFile::probe(char slot) {
  Probe result;
  char path[kMaxPath + 3];
  slotPath(slot, path);
  if (!fs_.exists(path)) return result;
  File f(fs_, path, IFs::Mode::Read);
  if (!f) return result;
  uint8_t header[kHeader];
  if (!f.readExact(header, kHeader) || memcmp(header, kMagic, 4) != 0) return result;
  ByteReader r(header + 4, kHeader - 4);
  const uint32_t seq = r.u32();
  const uint32_t len = r.u32();
  const uint32_t expect = r.u32();
  if (len > kMaxPayload || f.size() < static_cast<int64_t>(kHeader + len)) return result;
  uint32_t crc = crc32Update(crc32Begin(), header + 4, 8);
  uint8_t chunk[128];
  uint32_t left = len;
  while (left) {
    const size_t want = left < sizeof(chunk) ? left : sizeof(chunk);
    if (!f.readExact(chunk, want)) return result;
    crc = crc32Update(crc, chunk, want);
    left -= static_cast<uint32_t>(want);
  }
  if (crc32End(crc) != expect) return result;
  result.valid = true;
  result.seq = seq;
  result.len = len;
  return result;
}

void AtomicFile::refresh() {
  const Probe a = probe('a');
  const Probe b = probe('b');
  active_ = 0;
  seq_ = 0;
  if (a.valid && (!b.valid || a.seq >= b.seq)) {
    active_ = 'a';
    seq_ = a.seq;
  } else if (b.valid) {
    active_ = 'b';
    seq_ = b.seq;
  }
  probed_ = true;
}

bool AtomicFile::load(uint8_t* buf, size_t cap, size_t& len) {
  refresh();
  if (!active_) return false;
  char path[kMaxPath + 3];
  slotPath(active_, path);
  File f(fs_, path, IFs::Mode::Read);
  if (!f) return false;
  uint8_t header[kHeader];
  if (!f.readExact(header, kHeader)) return false;
  ByteReader r(header + 8, 4);
  const uint32_t stored = r.u32();
  if (stored > cap) return false;
  if (stored && !f.readExact(buf, stored)) return false;
  len = stored;
  return true;
}

bool AtomicFile::save(const void* payload, size_t len) {
  if (len > kMaxPayload) return false;
  if (!probed_) refresh();
  const char target = active_ == 'a' ? 'b' : 'a';
  const uint32_t seq = seq_ + 1;

  uint8_t header[kHeader];
  memcpy(header, kMagic, 4);
  ByteWriter w(header + 4, kHeader - 4);
  w.u32(seq);
  w.u32(static_cast<uint32_t>(len));
  uint32_t crc = crc32Update(crc32Begin(), header + 4, 8);
  crc = crc32Update(crc, payload, len);
  w.u32(crc32End(crc));

  char path[kMaxPath + 3];
  slotPath(target, path);
  // A failure below leaves the previous slot as the winner. Forget what we knew so
  // the next save probes again instead of trusting stale knowledge of the target.
  probed_ = false;
  File f(fs_, path, IFs::Mode::WriteTrunc);
  if (!f || !f.writeAll(header, kHeader) || !f.writeAll(payload, len) || !f.flush()) return false;
  active_ = target;
  seq_ = seq;
  probed_ = true;
  return true;
}

void AtomicFile::removeAll() {
  char path[kMaxPath + 3];
  slotPath('a', path);
  fs_.remove(path);
  slotPath('b', path);
  fs_.remove(path);
  active_ = 0;
  seq_ = 0;
  probed_ = true;
}

}  // namespace borges
