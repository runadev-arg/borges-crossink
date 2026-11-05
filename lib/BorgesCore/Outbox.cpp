#include "Outbox.h"

#include <cstring>
#include <new>

#include "Codec.h"

namespace borges {

namespace {

constexpr uint8_t kMagic0 = 0xB0;
constexpr uint8_t kMagic1 = 0x56;
constexpr uint8_t kTypeEvent = 1;
constexpr uint8_t kTypeAck = 2;
constexpr uint8_t kTypeAttempt = 3;
constexpr uint8_t kTypeRenumber = 4;
constexpr size_t kRecordHeader = 12;
// id + seq + book + kind + attempted + replaces(offset+1, 0 = none)
constexpr size_t kEventHeader = 16 + 8 + 16 + 1 + 1 + 4;
// {"client_event_id":"<36>","client_sequence":"<19>",  ...  }
constexpr size_t kEnvelopeOverhead = 20 + 36 + 22 + 19 + 3 + 2;

uint8_t headerCheck(uint8_t type, uint32_t len) {
  return static_cast<uint8_t>(type ^ 0x5A ^ len ^ (len >> 8) ^ (len >> 16) ^ (len >> 24));
}

void encodeHeader(uint8_t* out, uint8_t type, uint32_t len, uint32_t crc) {
  ByteWriter w(out, kRecordHeader);
  w.u8(kMagic0);
  w.u8(kMagic1);
  w.u8(type);
  w.u8(headerCheck(type, len));
  w.u32(len);
  w.u32(crc);
}

// CRC covers type and length too, so a record cannot be misread as another.
uint32_t crcBegin(uint8_t type, uint32_t len) {
  uint8_t prefix[5];
  ByteWriter w(prefix, sizeof(prefix));
  w.u8(type);
  w.u32(len);
  return crc32Update(crc32Begin(), prefix, sizeof(prefix));
}

}  // namespace

Outbox::Outbox(IFs& fs, IRandom& random, const char* dir) : fs_(fs), random_(random), meta_(fs, "") {
  copyText(dir_, sizeof(dir_), dir);
  char path[AtomicFile::kMaxPath + 16];
  const size_t n = strlen(dir_);
  memcpy(path, dir_, n);
  memcpy(path + n, "/outbox.meta", 13);
  meta_.setPath(path);
}

Outbox::~Outbox() { close(); }

void Outbox::close() {
  delete[] entries_;
  entries_ = nullptr;
  count_ = 0;
  capacity_ = 0;
  open_ = false;
}

void Outbox::logPath(uint32_t gen, char* out) const {
  const size_t n = strlen(dir_);
  memcpy(out, dir_, n);
  memcpy(out + n, "/outbox.", 8);
  const size_t digits = formatU64(gen, out + n + 8);
  memcpy(out + n + 8 + digits, ".log", 5);
}

bool Outbox::saveMeta(uint32_t gen) {
  uint8_t buf[12];
  ByteWriter w(buf, sizeof(buf));
  w.u32(gen);
  w.u64(nextSeq_);
  return meta_.save(buf, w.size());
}

bool Outbox::open() {
  close();
  fs_.mkdir(dir_);
  uint8_t buf[12];
  size_t len = 0;
  gen_ = 1;
  floor_ = 1;
  if (meta_.load(buf, sizeof(buf), len) && len == sizeof(buf)) {
    ByteReader r(buf, len);
    gen_ = r.u32();
    floor_ = r.u64();
  }
  // Leftovers of a compaction that crashed on either side of its commit point.
  char path[AtomicFile::kMaxPath + 16];
  logPath(gen_ + 1, path);
  fs_.remove(path);
  if (gen_ > 1) {
    logPath(gen_ - 1, path);
    fs_.remove(path);
  }
  capacity_ = 16;
  entries_ = new (std::nothrow) OutboxEntry[capacity_];
  if (!entries_) {
    capacity_ = 0;
    return false;
  }
  if (!scan()) {
    close();
    return false;
  }
  open_ = true;
  if (needsCompaction_) compact();  // Failing here is harmless: the scan is repeatable.
  return open_;
}

bool Outbox::addEntry(const OutboxEntry& entry) {
  if (count_ == capacity_) {
    const uint16_t next = static_cast<uint16_t>(capacity_ * 2);
    OutboxEntry* grown = new (std::nothrow) OutboxEntry[next];
    if (!grown) return false;
    memcpy(grown, entries_, sizeof(OutboxEntry) * count_);
    delete[] entries_;
    entries_ = grown;
    capacity_ = next;
  }
  entries_[count_++] = entry;
  return true;
}

OutboxEntry* Outbox::findEntry(uint32_t offset) {
  size_t lo = 0, hi = count_;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (entries_[mid].offset == offset) return &entries_[mid];
    if (entries_[mid].offset < offset) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return nullptr;
}

void Outbox::removeEntry(uint32_t offset) {
  OutboxEntry* e = findEntry(offset);
  if (!e) return;
  const size_t index = static_cast<size_t>(e - entries_);
  memmove(e, e + 1, sizeof(OutboxEntry) * (count_ - index - 1));
  --count_;
}

// Whether a complete, checksummed record starts at pos. A failed read is its own answer:
// treating it as corruption would quarantine good events on a passing SD error.
Outbox::Verify Outbox::verifyRecord(File& f, uint32_t pos, uint32_t fileSize, uint8_t& type, uint32_t& len) {
  if (fileSize - pos < kRecordHeader) return Verify::Invalid;
  uint8_t header[kRecordHeader];
  if (!f.seek(pos) || !f.readExact(header, kRecordHeader)) return Verify::IoError;
  if (header[0] != kMagic0 || header[1] != kMagic1) return Verify::Invalid;
  ByteReader r(header + 4, 8);
  len = r.u32();
  const uint32_t expect = r.u32();
  type = header[2];
  if (header[3] != headerCheck(type, len) || len > kMaxRecordBytes || fileSize - pos - kRecordHeader < len) {
    return Verify::Invalid;
  }
  uint32_t crc = crcBegin(type, len);
  uint8_t chunk[128];
  uint32_t left = len;
  while (left) {
    const size_t want = left < sizeof(chunk) ? left : sizeof(chunk);
    if (!f.readExact(chunk, want)) return Verify::IoError;
    crc = crc32Update(crc, chunk, want);
    left -= static_cast<uint32_t>(want);
  }
  return crc32End(crc) == expect ? Verify::Valid : Verify::Invalid;
}

void Outbox::quarantineRange(File& source, uint32_t start, uint32_t len) {
  char path[AtomicFile::kMaxPath + 16];
  const size_t n = strlen(dir_);
  memcpy(path, dir_, n);
  memcpy(path + n, "/outbox.bad", 12);
  File bad(fs_, path, IFs::Mode::Append);
  if (!bad) return;
  const int64_t existing = bad.size();
  if (existing < 0 || existing >= kMaxBadBytes) return;
  if (len > kMaxBadBytes - static_cast<uint32_t>(existing)) len = kMaxBadBytes - static_cast<uint32_t>(existing);
  if (!source.seek(start)) return;
  uint8_t chunk[128];
  while (len) {
    const size_t want = len < sizeof(chunk) ? len : sizeof(chunk);
    if (!source.readExact(chunk, want) || !bad.writeAll(chunk, want)) return;
    len -= static_cast<uint32_t>(want);
  }
  bad.flush();
}

bool Outbox::scan() {
  count_ = 0;
  needsCompaction_ = false;
  nextSeq_ = floor_;
  logSize_ = 0;
  char path[AtomicFile::kMaxPath + 16];
  logPath(gen_, path);
  if (!fs_.exists(path)) return true;
  File f(fs_, path, IFs::Mode::Read);
  if (!f) return false;
  const int64_t rawSize = f.size();
  if (rawSize < 0) return false;
  const uint32_t size = static_cast<uint32_t>(rawSize);
  logSize_ = size;

  uint32_t pos = 0;
  uint32_t badStart = 0;
  bool inBad = false;
  const auto endBad = [&](uint32_t end) {
    if (!inBad) return;
    inBad = false;
    quarantineRange(f, badStart, end - badStart);
    ++quarantined_;
    needsCompaction_ = true;
  };

  while (pos < size) {
    uint8_t type = 0;
    uint32_t len = 0;
    const Verify verdict = verifyRecord(f, pos, size, type, len);
    if (verdict == Verify::IoError) return false;  // Nothing is decided on a failed read.
    if (verdict == Verify::Invalid) {
      if (!inBad) {
        inBad = true;
        badStart = pos;
      }
      // Resynchronize on the next magic byte: a torn record's length can run over
      // later valid records, so it cannot be trusted to skip ahead.
      ++pos;
      uint8_t chunk[128];
      bool found = false;
      while (!found && pos < size) {
        if (!f.seek(pos)) return false;
        const int32_t got = f.read(chunk, sizeof(chunk));
        if (got <= 0) return false;
        for (int32_t i = 0; i < got && !found; ++i) {
          if (chunk[i] == kMagic0) {
            pos += static_cast<uint32_t>(i);
            found = true;
          }
        }
        if (!found) pos += static_cast<uint32_t>(got);
      }
      continue;
    }
    endBad(pos);
    const uint32_t payload = pos + kRecordHeader;
    if (!f.seek(payload)) return false;
    if (type == kTypeEvent && len >= kEventHeader) {
      uint8_t head[kEventHeader];
      if (!f.readExact(head, kEventHeader)) return false;
      ByteReader r(head, kEventHeader);
      uint8_t id[16], book[16];
      r.bytes(id, 16);
      OutboxEntry e{};
      e.offset = pos;
      e.payloadLen = len;
      e.seq = r.u64();
      r.bytes(book, 16);
      e.bookTag = bookTagOf(book);
      e.kind = r.u8();
      e.attempted = r.u8();
      const uint32_t replaces = r.u32();
      if (replaces) removeEntry(replaces - 1);
      if (!addEntry(e)) return false;
      if (e.seq >= nextSeq_) nextSeq_ = e.seq + 1;
    } else if (type == kTypeAck || type == kTypeAttempt) {
      for (uint32_t i = 0; i + 4 <= len; i += 4) {
        uint8_t raw[4];
        if (!f.readExact(raw, 4)) return false;
        ByteReader r(raw, 4);
        const uint32_t offset = r.u32();
        if (type == kTypeAck) {
          removeEntry(offset);
        } else if (OutboxEntry* e = findEntry(offset)) {
          e->attempted = 1;
        }
      }
    } else if (type == kTypeRenumber) {
      for (uint32_t i = 0; i + 12 <= len; i += 12) {
        uint8_t raw[12];
        if (!f.readExact(raw, 12)) return false;
        ByteReader r(raw, 12);
        const uint32_t offset = r.u32();
        const uint64_t seq = r.u64();
        if (OutboxEntry* e = findEntry(offset)) e->seq = seq;
        if (seq >= nextSeq_) nextSeq_ = seq + 1;
      }
    }
    pos = payload + len;
  }
  endBad(size);

  uint32_t liveBytes = 0;
  for (size_t i = 0; i < count_; ++i) liveBytes += static_cast<uint32_t>(kRecordHeader) + entries_[i].payloadLen;
  if (size - liveBytes >= kCompactDeadBytes) needsCompaction_ = true;
  return true;
}

Outbox::AppendResult Outbox::append(EventKind kind, const uint8_t md5[16], const char* body, size_t bodyLen,
                                    bool replaceUnattempted, uint8_t outId[16], uint64_t* outSeq) {
  if (!open_) return AppendResult::NotOpen;
  const uint32_t payloadLen = static_cast<uint32_t>(kEventHeader + bodyLen);
  if (bodyLen > kMaxRecordBytes - kEventHeader) return AppendResult::TooLarge;
  const uint32_t tag = bookTagOf(md5);

  uint32_t replaces = 0;
  if (replaceUnattempted) {
    for (size_t i = count_; i-- > 0;) {
      const OutboxEntry& e = entries_[i];
      if (e.kind == static_cast<uint8_t>(kind) && e.bookTag == tag && !e.attempted) {
        replaces = e.offset + 1;
        break;
      }
    }
  }
  if (!replaces && count_ >= kMaxLiveEvents) return AppendResult::Full;

  uint8_t id[16];
  uuidV4(random_, id);
  const uint64_t seq = nextSeq_;

  uint8_t head[kEventHeader];
  ByteWriter w(head, sizeof(head));
  w.bytes(id, 16);
  w.u64(seq);
  w.bytes(md5, 16);
  w.u8(static_cast<uint8_t>(kind));
  w.u8(0);
  w.u32(replaces);

  uint32_t crc = crcBegin(kTypeEvent, payloadLen);
  crc = crc32Update(crc, head, sizeof(head));
  crc = crc32Update(crc, body, bodyLen);
  uint8_t recordHeader[kRecordHeader];
  encodeHeader(recordHeader, kTypeEvent, payloadLen, crc32End(crc));

  // The number is consumed even if the write fails: those bytes may reach the disk.
  nextSeq_ = seq + 1;

  char path[AtomicFile::kMaxPath + 16];
  logPath(gen_, path);
  File f(fs_, path, IFs::Mode::Append);
  if (!f) return AppendResult::IoError;
  const int64_t rawSize = f.size();
  if (rawSize < 0) return AppendResult::IoError;
  const uint32_t offset = static_cast<uint32_t>(rawSize);
  if (!f.writeAll(recordHeader, kRecordHeader) || !f.writeAll(head, sizeof(head)) || !f.writeAll(body, bodyLen) ||
      !f.flush()) {
    needsCompaction_ = true;
    return AppendResult::IoError;
  }
  f.close();

  if (replaces) removeEntry(replaces - 1);
  OutboxEntry e{};
  e.offset = offset;
  e.payloadLen = payloadLen;
  e.seq = seq;
  e.bookTag = tag;
  e.kind = static_cast<uint8_t>(kind);
  e.attempted = 0;
  if (!addEntry(e)) {
    // Durable but not indexed: the next open() finds it. Nothing is lost.
    open_ = false;
    return AppendResult::IoError;
  }
  logSize_ = offset + static_cast<uint32_t>(kRecordHeader) + payloadLen;
  if (outId) memcpy(outId, id, 16);
  if (outSeq) *outSeq = seq;
  return AppendResult::Ok;
}

size_t Outbox::sendableDepth(const IHoldFilter* holds) const {
  size_t n = 0;
  for (size_t i = 0; i < count_; ++i) {
    const OutboxEntry& e = entries_[i];
    if (holds && e.kind == static_cast<uint8_t>(EventKind::Progress) && holds->isHeld(e.bookTag)) continue;
    ++n;
  }
  return n;
}

bool Outbox::hasEventFor(uint32_t bookTag, EventKind kind) const {
  for (size_t i = 0; i < count_; ++i) {
    if (entries_[i].bookTag == bookTag && entries_[i].kind == static_cast<uint8_t>(kind)) return true;
  }
  return false;
}

void Outbox::selectBatch(size_t maxEvents, size_t maxBytes, const IHoldFilter* holds, const uint32_t* skip,
                         size_t skipCount, Batch& out) const {
  out.count = 0;
  out.wireBytes = 0;
  if (maxEvents > kMaxBatchEvents) maxEvents = kMaxBatchEvents;
  for (size_t i = 0; i < count_ && out.count < maxEvents; ++i) {
    const OutboxEntry& e = entries_[i];
    if (holds && e.kind == static_cast<uint8_t>(EventKind::Progress) && holds->isHeld(e.bookTag)) continue;
    bool skipped = false;
    for (size_t s = 0; s < skipCount && !skipped; ++s) skipped = skip[s] == e.offset;
    if (skipped) continue;
    const size_t wire = e.payloadLen - kEventHeader + kEnvelopeOverhead + 1;
    if (out.count > 0 && out.wireBytes + wire > maxBytes) break;
    BatchItem& item = out.items[out.count++];
    memset(item.id, 0, sizeof(item.id));
    item.seq = e.seq;
    item.offset = e.offset;
    out.wireBytes += wire;
  }
}

bool Outbox::appendOffsets(uint8_t type, const uint32_t* offsets, size_t count) {
  const uint32_t len = static_cast<uint32_t>(count * 4);
  uint32_t crc = crcBegin(type, len);
  uint8_t raw[4];
  for (size_t i = 0; i < count; ++i) {
    ByteWriter w(raw, 4);
    w.u32(offsets[i]);
    crc = crc32Update(crc, raw, 4);
  }
  uint8_t header[kRecordHeader];
  encodeHeader(header, type, len, crc32End(crc));
  char path[AtomicFile::kMaxPath + 16];
  logPath(gen_, path);
  File f(fs_, path, IFs::Mode::Append);
  if (!f || !f.writeAll(header, kRecordHeader)) return false;
  for (size_t i = 0; i < count; ++i) {
    ByteWriter w(raw, 4);
    w.u32(offsets[i]);
    if (!f.writeAll(raw, 4)) return false;
  }
  if (!f.flush()) return false;
  logSize_ += static_cast<uint32_t>(kRecordHeader) + len;
  return true;
}

bool Outbox::markAttempted(const Batch& batch) {
  uint32_t offsets[kMaxBatchEvents];
  size_t n = 0;
  for (size_t i = 0; i < batch.count; ++i) {
    const OutboxEntry* e = findEntry(batch.items[i].offset);
    if (e && !e->attempted) offsets[n++] = e->offset;
  }
  if (n == 0) return true;
  if (!appendOffsets(kTypeAttempt, offsets, n)) return false;
  for (size_t i = 0; i < n; ++i) {
    if (OutboxEntry* e = findEntry(offsets[i])) e->attempted = 1;
  }
  return true;
}

Outbox::EnvelopeResult Outbox::writeEnvelope(BatchItem& item, JsonWriter& out) {
  const OutboxEntry* entry = findEntry(item.offset);
  if (!entry) return EnvelopeResult::IoError;
  char path[AtomicFile::kMaxPath + 16];
  logPath(gen_, path);
  File f(fs_, path, IFs::Mode::Read);
  if (!f) return EnvelopeResult::IoError;
  const int64_t rawSize = f.size();
  if (rawSize < 0) return EnvelopeResult::IoError;
  uint8_t type = 0;
  uint32_t len = 0;
  const Verify verdict = verifyRecord(f, item.offset, static_cast<uint32_t>(rawSize), type, len);
  if (verdict == Verify::IoError) return EnvelopeResult::IoError;
  if (verdict == Verify::Invalid || type != kTypeEvent || len < kEventHeader) {
    f.close();
    return quarantine(item.offset) ? EnvelopeResult::Quarantined : EnvelopeResult::IoError;
  }
  uint8_t head[kEventHeader];
  if (!f.seek(item.offset + kRecordHeader) || !f.readExact(head, kEventHeader)) return EnvelopeResult::IoError;
  uint8_t id[16];
  memcpy(id, head, 16);
  out.objectBegin();
  out.key("client_event_id");
  out.valueUuid(id);
  out.kvSeq("client_sequence", entry->seq);
  out.raw(",");
  char chunk[128];
  uint32_t left = len - static_cast<uint32_t>(kEventHeader);
  while (left) {
    const size_t want = left < sizeof(chunk) ? left : sizeof(chunk);
    if (!f.readExact(chunk, want)) return EnvelopeResult::IoError;
    out.rawBytes(chunk, want);
    left -= static_cast<uint32_t>(want);
  }
  out.objectEnd();
  // The id goes back through the item: the caller matches ACKs against it.
  memcpy(item.id, id, 16);
  item.seq = entry->seq;
  return EnvelopeResult::Ok;
}

bool Outbox::ack(const uint32_t* offsets, size_t count) {
  if (count == 0) return true;
  if (!appendOffsets(kTypeAck, offsets, count)) return false;
  for (size_t i = 0; i < count; ++i) removeEntry(offsets[i]);
  return true;
}

bool Outbox::quarantine(uint32_t offset) {
  const OutboxEntry* e = findEntry(offset);
  if (!e) return true;
  char path[AtomicFile::kMaxPath + 16];
  logPath(gen_, path);
  {
    File f(fs_, path, IFs::Mode::Read);
    if (!f) return false;
    quarantineRange(f, offset, static_cast<uint32_t>(kRecordHeader) + e->payloadLen);
  }
  ++quarantined_;
  return ack(&offset, 1);
}

bool Outbox::renumber(const uint32_t* proven, size_t provenCount, uint64_t floor) {
  uint64_t seq = nextSeq_ > floor ? nextSeq_ : floor;
  const auto selected = [&](const OutboxEntry& e) {
    if (!e.attempted) return true;
    for (size_t p = 0; p < provenCount; ++p) {
      if (proven[p] == e.offset) return true;
    }
    return false;
  };
  uint32_t len = 0;
  for (size_t i = 0; i < count_; ++i) {
    if (selected(entries_[i])) len += 12;
  }
  if (len == 0) return false;
  uint8_t raw[12];
  uint32_t crc = crcBegin(kTypeRenumber, len);
  uint64_t assign = seq;
  for (size_t i = 0; i < count_; ++i) {
    if (!selected(entries_[i])) continue;
    ByteWriter w(raw, sizeof(raw));
    w.u32(entries_[i].offset);
    w.u64(assign++);
    crc = crc32Update(crc, raw, sizeof(raw));
  }
  uint8_t header[kRecordHeader];
  encodeHeader(header, kTypeRenumber, len, crc32End(crc));
  char path[AtomicFile::kMaxPath + 16];
  logPath(gen_, path);
  File f(fs_, path, IFs::Mode::Append);
  if (!f || !f.writeAll(header, kRecordHeader)) return false;
  assign = seq;
  for (size_t i = 0; i < count_; ++i) {
    if (!selected(entries_[i])) continue;
    ByteWriter w(raw, sizeof(raw));
    w.u32(entries_[i].offset);
    w.u64(assign++);
    if (!f.writeAll(raw, sizeof(raw))) return false;
  }
  if (!f.flush()) return false;
  logSize_ += static_cast<uint32_t>(kRecordHeader) + len;
  assign = seq;
  for (size_t i = 0; i < count_; ++i) {
    if (selected(entries_[i])) entries_[i].seq = assign++;
  }
  nextSeq_ = assign;
  return true;
}

bool Outbox::compactIfNeeded() {
  if (!open_) return false;
  uint32_t liveBytes = 0;
  for (size_t i = 0; i < count_; ++i) liveBytes += static_cast<uint32_t>(kRecordHeader) + entries_[i].payloadLen;
  if (!needsCompaction_ && logSize_ - liveBytes < kCompactDeadBytes) return true;
  return compact();
}

bool Outbox::compact() {
  char oldPath[AtomicFile::kMaxPath + 16];
  char newPath[AtomicFile::kMaxPath + 16];
  logPath(gen_, oldPath);
  logPath(gen_ + 1, newPath);
  bool ok = true;
  {
    File dst(fs_, newPath, IFs::Mode::WriteTrunc);
    ok = static_cast<bool>(dst);
    if (ok && count_ > 0) {
      File src(fs_, oldPath, IFs::Mode::Read);
      const int64_t rawSize = src ? src.size() : -1;
      ok = rawSize >= 0;
      for (size_t i = 0; ok && i < count_; ++i) {
        const OutboxEntry& e = entries_[i];
        uint8_t type = 0;
        uint32_t len = 0;
        const Verify verdict = verifyRecord(src, e.offset, static_cast<uint32_t>(rawSize), type, len);
        if (verdict == Verify::IoError) {
          ok = false;
          break;
        }
        if (verdict == Verify::Invalid || type != kTypeEvent || len != e.payloadLen) {
          // Went bad since the scan. A later scan of this generation will not see it.
          quarantineRange(src, e.offset, static_cast<uint32_t>(kRecordHeader) + e.payloadLen);
          ++quarantined_;
          continue;
        }
        uint8_t head[kEventHeader];
        ok = src.seek(e.offset + kRecordHeader) && src.readExact(head, kEventHeader);
        if (!ok) break;
        // Fold what the control records said into the event itself.
        ByteWriter patch(head + 16, 8);
        patch.u64(e.seq);
        head[16 + 8 + 16 + 1] = e.attempted;
        memset(head + 16 + 8 + 16 + 2, 0, 4);
        uint32_t crc = crcBegin(kTypeEvent, len);
        crc = crc32Update(crc, head, kEventHeader);
        uint8_t chunk[128];
        const uint32_t body = len - static_cast<uint32_t>(kEventHeader);
        uint32_t left = body;
        while (ok && left) {
          const size_t want = left < sizeof(chunk) ? left : sizeof(chunk);
          ok = src.readExact(chunk, want);
          if (ok) crc = crc32Update(crc, chunk, want);
          left -= static_cast<uint32_t>(want);
        }
        if (!ok) break;
        uint8_t header[kRecordHeader];
        encodeHeader(header, kTypeEvent, len, crc32End(crc));
        ok = dst.writeAll(header, kRecordHeader) && dst.writeAll(head, kEventHeader) &&
             src.seek(e.offset + kRecordHeader + kEventHeader);
        left = body;
        while (ok && left) {
          const size_t want = left < sizeof(chunk) ? left : sizeof(chunk);
          ok = src.readExact(chunk, want) && dst.writeAll(chunk, want);
          left -= static_cast<uint32_t>(want);
        }
      }
    }
    ok = ok && dst.flush();
  }
  // The commit point: from here on the new generation is the queue.
  if (ok) ok = saveMeta(gen_ + 1);
  if (!ok) {
    fs_.remove(newPath);
    return false;
  }
  fs_.remove(oldPath);
  gen_ += 1;
  floor_ = nextSeq_;
  if (!scan()) {
    open_ = false;
    return false;
  }
  return true;
}

bool Outbox::reset() {
  if (!open_) return false;
  char oldPath[AtomicFile::kMaxPath + 16];
  logPath(gen_, oldPath);
  if (!saveMeta(gen_ + 1)) return false;
  fs_.remove(oldPath);
  gen_ += 1;
  floor_ = nextSeq_;
  return scan();
}

}  // namespace borges
