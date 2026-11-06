#include "Inbound.h"

#include <cstring>
#include <new>

#include "Codec.h"
#include "Spool.h"

namespace borges {

namespace {

constexpr uint8_t kLogMagic[4] = {'B', 'I', 'N', '1'};
constexpr size_t kLogHeaderBytes = 20;  // magic, epoch u16, dropped u16, watermark u64, crc u32.
constexpr uint8_t kFrameMagic0 = 0xB1;
constexpr uint8_t kFrameMagic1 = 0x57;
constexpr size_t kFrameHeaderBytes = 8;  // magic u16, length u16, crc u32.
constexpr uint8_t kKindHighlight = 1;
constexpr uint8_t kKindDeleted = 2;
constexpr uint8_t kKindSeal = 3;
constexpr uint8_t kFlagTruncated = 1;
constexpr uint8_t kFlagHasPct = 2;
constexpr size_t kFixedBytes = 1 + 1 + 4 + 8 + 16 + 4 + 8 + 8;
constexpr size_t kMaxRecordBytes =
    kFixedBytes + 8 + kMaxXpointer + kInboundChapterMax + kInboundDeviceMax + kInboundTextMax;
constexpr size_t kPassageBytes = 200;

constexpr uint8_t kIndexVersion = 1;
constexpr size_t kIndexHeaderBytes = 5;  // version u8, epoch u16, count u16.
constexpr size_t kIndexEntryBytes = 20;  // md5, bytes u32.
constexpr size_t kIndexBytes = kIndexHeaderBytes + (kInboundMaxBooks + 1u) * kIndexEntryBytes;
constexpr const char* kIndexPath = "/.crosspoint/borges/inbound/index";

constexpr size_t kPathCap = AtomicFile::kMaxPath;

void logPath(const BookId& id, char* out) {
  const size_t a = strlen(kInboundDir);
  memcpy(out, kInboundDir, a);
  out[a] = '/';
  out[a + 1] = id.hex[0];
  out[a + 2] = id.hex[1];
  out[a + 3] = '/';
  memcpy(out + a + 4, id.hex + 2, 31);
}

void asidePath(const char* path, char* out) {
  const size_t n = strlen(path);
  memcpy(out, path, n);
  memcpy(out + n, ".n", 3);
}

void shardDir(const char* path, char* out) {
  const char* slash = strrchr(path, '/');
  const size_t n = static_cast<size_t>(slash - path);
  memcpy(out, path, n);
  out[n] = '\0';
}

enum class LogHeader : uint8_t { Good, NotALog, Unreadable };

// A log's first bytes. NotALog: torn before it was complete, which is a log nobody lost anything
// in. Unreadable says nothing about the log, and must not lead to writing over it.
LogHeader readLogHeader(File& f, uint16_t& epoch, uint16_t& dropped, uint64_t& watermark) {
  const int64_t size = f.size();
  if (size < 0) return LogHeader::Unreadable;
  if (size < static_cast<int64_t>(kLogHeaderBytes)) return LogHeader::NotALog;
  uint8_t header[kLogHeaderBytes];
  if (!f.seek(0) || !f.readExact(header, sizeof(header))) return LogHeader::Unreadable;
  if (memcmp(header, kLogMagic, 4) != 0) return LogHeader::NotALog;
  ByteReader r(header + 4, sizeof(header) - 4);
  epoch = r.u16();
  dropped = r.u16();
  watermark = r.u64();
  return r.u32() == crc32(header, kLogHeaderBytes - 4) ? LogHeader::Good : LogHeader::NotALog;
}

bool writeLogHeader(File& f, const uint16_t epoch, const uint16_t dropped, const uint64_t watermark) {
  uint8_t header[kLogHeaderBytes];
  memcpy(header, kLogMagic, 4);
  ByteWriter w(header + 4, sizeof(header) - 4);
  w.u16(epoch);
  w.u16(dropped);
  w.u64(watermark);
  w.u32(crc32(header, kLogHeaderBytes - 4));
  return f.writeAll(header, sizeof(header));
}

// Walks the valid frames of a log, resynchronizing past whatever a crash left. buffer holds
// kMaxRecordBytes.
class LogWalker {
 public:
  LogWalker(File& f, uint8_t* buffer) : f_(f), buffer_(buffer) {
    const int64_t size = f.size();
    size_ = size < 0 ? 0 : static_cast<uint32_t>(size);
    pos_ = kLogHeaderBytes;
  }

  // True with the frame's payload in the buffer.
  bool next() {
    while (pos_ + kFrameHeaderBytes <= size_) {
      uint8_t header[kFrameHeaderBytes];
      if (!f_.seek(pos_) || !f_.readExact(header, sizeof(header))) {
        pos_ = size_;
        ioError_ = true;
        return false;
      }
      ByteReader r(header + 2, sizeof(header) - 2);
      const uint16_t len = r.u16();
      const uint32_t crc = r.u32();
      if (header[0] == kFrameMagic0 && header[1] == kFrameMagic1 && len >= 1 && len <= kMaxRecordBytes &&
          pos_ + kFrameHeaderBytes + len <= size_) {
        if (!f_.readExact(buffer_, len)) {
          pos_ = size_;
          ioError_ = true;
          return false;
        }
        if (crc32(buffer_, len) == crc) {
          offset_ = pos_;
          length_ = len;
          pos_ += kFrameHeaderBytes + len;
          return true;
        }
      }
      damaged_ = true;
      if (!resync()) return false;
    }
    if (pos_ < size_) damaged_ = true;  // Too short to be a frame: torn.
    return false;
  }

  uint32_t offset() const { return offset_; }
  uint16_t length() const { return length_; }
  bool damaged() const { return damaged_; }
  bool ioError() const { return ioError_; }

 private:
  // A torn frame's length means nothing: on to the next pair of magic bytes.
  bool resync() {
    uint8_t chunk[64];
    uint32_t at = pos_ + 1;
    while (at + 1 < size_) {
      const uint32_t left = size_ - at;
      const size_t want = left < sizeof(chunk) ? left : sizeof(chunk);
      if (!f_.seek(at) || !f_.readExact(chunk, want)) {
        ioError_ = true;
        pos_ = size_;
        return false;
      }
      for (size_t i = 0; i + 1 < want; ++i) {
        if (chunk[i] == kFrameMagic0 && chunk[i + 1] == kFrameMagic1) {
          pos_ = at + static_cast<uint32_t>(i);
          return true;
        }
      }
      if (want < 2) break;
      at += static_cast<uint32_t>(want - 1);
    }
    pos_ = size_;
    return false;
  }

  File& f_;
  uint8_t* buffer_;
  uint32_t size_ = 0;
  uint32_t pos_ = 0;
  uint32_t offset_ = 0;
  uint16_t length_ = 0;
  bool damaged_ = false;
  bool ioError_ = false;
};

void frameHeader(const uint8_t* payload, const size_t len, uint8_t out[kFrameHeaderBytes]) {
  out[0] = kFrameMagic0;
  out[1] = kFrameMagic1;
  ByteWriter w(out + 2, kFrameHeaderBytes - 2);
  w.u16(static_cast<uint16_t>(len));
  w.u32(crc32(payload, len));
}

size_t encodeHighlight(const InboundHighlight& h, uint8_t* buffer) {
  ByteWriter w(buffer, kMaxRecordBytes);
  w.u8(h.deleted ? kKindDeleted : kKindHighlight);
  w.u8(static_cast<uint8_t>((h.truncated ? kFlagTruncated : 0) | (h.hasPct ? kFlagHasPct : 0)));
  w.u32(h.revision);
  w.u64(h.serverSeq);
  w.bytes(h.syncId, 16);
  if (!h.deleted) {
    w.u32(h.pctMicro);
    w.u64(static_cast<uint64_t>(h.timeEpoch));
    w.u64(h.passage);
    w.str(h.xpointer);
    w.str(h.chapter);
    w.str(h.device);
    w.str(h.text);
  }
  return w.ok() ? w.size() : 0;
}

// The first few fields, which is all a walk over the log needs.
struct RecordHead {
  uint8_t kind = 0;
  uint32_t revision = 0;
  uint64_t serverSeq = 0;
  uint8_t tag[6] = {0};
};

bool readHead(const uint8_t* payload, const size_t len, RecordHead& out) {
  if (len < 1) return false;
  out.kind = payload[0];
  if (out.kind == kKindSeal) return true;
  if (len < 1 + 1 + 4 + 8 + 16) return false;
  ByteReader r(payload + 2, len - 2);
  out.revision = r.u32();
  out.serverSeq = r.u64();
  uint8_t syncId[16];
  r.bytes(syncId, sizeof(syncId));
  // A hash: an id may say more in some bytes than in others. 48 bits among 256 ids in a book.
  const uint64_t hash = fnv1a64(syncId, sizeof(syncId));
  memcpy(out.tag, &hash, sizeof(out.tag));
  return r.ok();
}

bool decodeHighlight(const uint8_t* payload, const size_t len, InboundHighlight& out) {
  out = InboundHighlight{};
  ByteReader r(payload, len);
  const uint8_t kind = r.u8();
  if (kind != kKindHighlight && kind != kKindDeleted) return false;
  const uint8_t flags = r.u8();
  out.deleted = kind == kKindDeleted;
  out.truncated = (flags & kFlagTruncated) != 0;
  out.hasPct = (flags & kFlagHasPct) != 0;
  out.revision = r.u32();
  out.serverSeq = r.u64();
  r.bytes(out.syncId, 16);
  if (!out.deleted) {
    out.pctMicro = r.u32();
    out.timeEpoch = static_cast<int64_t>(r.u64());
    out.passage = r.u64();
    r.str(out.xpointer, sizeof(out.xpointer));
    r.str(out.chapter, sizeof(out.chapter));
    r.str(out.device, sizeof(out.device));
    r.str(out.text, sizeof(out.text));
  }
  return r.ok();
}

// What a log says when its updates and deletes are folded into one line per highlight.
struct Row {
  uint8_t tag[6];
  uint16_t length;
  uint32_t offset;
  uint32_t revision;
};
constexpr uint16_t kRowDeleted = 0x8000;

struct Fold {
  Row* rows = nullptr;
  uint16_t count = 0;
  uint16_t records = 0;
  uint16_t dropped = 0;
  uint32_t bytes = 0;
  uint64_t watermark = 0;
  bool usable = false;  // The log is whole and of this epoch.
  bool damaged = false;
  bool sealed = false;
};

// rows may be null, to only learn the totals. buffer holds kMaxRecordBytes. False on IO.
bool foldLog(IFs& fs, const char* path, const uint16_t epoch, uint8_t* buffer, Row* rows, const uint16_t rowCap,
             Fold& out) {
  out = Fold{};
  out.rows = rows;
  File f(fs, path, IFs::Mode::Read);
  if (!f) return !fs.exists(path);
  uint16_t stored = 0;
  uint16_t dropped = 0;
  uint64_t watermark = 0;
  const LogHeader header = readLogHeader(f, stored, dropped, watermark);
  if (header == LogHeader::Unreadable) return false;
  if (header == LogHeader::NotALog || stored != epoch) return true;
  out.usable = true;
  out.dropped = dropped;
  out.watermark = watermark;
  const int64_t size = f.size();
  out.bytes = size < 0 ? 0 : static_cast<uint32_t>(size);
  LogWalker walker(f, buffer);
  while (walker.next()) {
    RecordHead head;
    if (!readHead(buffer, walker.length(), head)) continue;
    if (head.kind == kKindSeal) {
      out.sealed = true;
      continue;
    }
    out.sealed = false;
    ++out.records;
    if (head.serverSeq > out.watermark) out.watermark = head.serverSeq;
    if (!rows) continue;
    Row* row = nullptr;
    for (uint16_t i = 0; i < out.count && !row; ++i) {
      if (memcmp(rows[i].tag, head.tag, sizeof(head.tag)) == 0) row = &rows[i];
    }
    if (row) {
      // An older revision that arrives later says nothing new. A server that did not say
      // leaves the order of arrival to decide.
      if (row->revision && head.revision && head.revision < row->revision) continue;
    } else {
      if (out.count >= rowCap) continue;
      row = &rows[out.count++];
      memcpy(row->tag, head.tag, sizeof(head.tag));
    }
    row->revision = head.revision;
    row->offset = walker.offset();
    row->length = static_cast<uint16_t>(walker.length() | (head.kind == kKindDeleted ? kRowDeleted : 0));
  }
  out.damaged = walker.damaged();
  return !walker.ioError();
}

void sortByOffset(Row* rows, const uint16_t count) {
  // At most 256, almost in order already: the fold adds them as they first appear.
  for (uint16_t i = 1; i < count; ++i) {
    const Row key = rows[i];
    uint16_t j = i;
    while (j > 0 && rows[j - 1].offset > key.offset) {
      rows[j] = rows[j - 1];
      --j;
    }
    rows[j] = key;
  }
}

// A compaction a crash finished writing wins over the log it replaces. One it did not finish goes.
bool recoverAside(IFs& fs, const char* path, const uint16_t epoch, uint8_t* buffer) {
  char aside[kPathCap + 3];
  asidePath(path, aside);
  if (!fs.exists(aside)) return true;
  Fold fold;
  if (!foldLog(fs, aside, epoch, buffer, nullptr, 0, fold)) return false;
  if (fold.usable && fold.sealed) {
    if (!fs.remove(path)) return false;
    return fs.rename(aside, path);
  }
  return fs.remove(aside);
}

// Parses the index into the caller's kIndexBytes. count 0 for one that is not there.
bool readIndex(AtomicFile& file, uint8_t* buffer, uint16_t& epoch, uint16_t& count) {
  epoch = 0;
  count = 0;
  size_t len = 0;
  const bool loaded = file.load(buffer, kIndexBytes, len);
  if (file.ioError()) return false;
  if (!loaded || len < kIndexHeaderBytes || buffer[0] != kIndexVersion) return true;
  ByteReader r(buffer + 1, 4);
  epoch = r.u16();
  const uint16_t n = r.u16();
  if (n <= kInboundMaxBooks && len == kIndexHeaderBytes + n * kIndexEntryBytes) count = n;
  return true;
}

void removeBook(IFs& fs, const uint8_t md5[16]) {
  BookId id;
  id.setRaw(md5);
  char path[kPathCap];
  logPath(id, path);
  char aside[kPathCap + 3];
  asidePath(path, aside);
  // Removed as any file on FAT is: the clusters go free, unwritten. Rewriting up to 4 MiB to wipe them
  // would make taking over a card take minutes.
  fs.remove(aside);
  fs.remove(path);
}

uint32_t entryBytes(const uint8_t* entry) {
  ByteReader r(entry + 16, 4);
  return r.u32();
}

void setEntryBytes(uint8_t* entry, const uint32_t bytes) {
  ByteWriter w(entry + 16, 4);
  w.u32(bytes);
}

}  // namespace

uint64_t passageHash(const char* text) {
  if (!text) return 0;
  uint64_t hash = kFnvBasis;
  size_t used = 0;
  bool pendingSpace = false;
  for (const char* p = text; *p && used < kPassageBytes;) {
    const auto c = static_cast<unsigned char>(*p);
    if (c == 0xC2 && static_cast<unsigned char>(p[1]) == 0xAD) {  // Soft hyphen.
      p += 2;
      continue;
    }
    const bool nbsp = c == 0xC2 && static_cast<unsigned char>(p[1]) == 0xA0;
    if (nbsp || c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\v') {
      pendingSpace = used > 0;
      p += nbsp ? 2 : 1;
      continue;
    }
    if (pendingSpace) {
      const char space = ' ';
      hash = fnv1a64(&space, 1, hash);
      ++used;
      pendingSpace = false;
    }
    hash = fnv1a64(p, 1, hash);
    ++used;
    ++p;
  }
  return used ? hash : 0;
}

int32_t spineOfXpointer(const char* xpointer) {
  static constexpr char kPrefix[] = "/body/DocFragment";
  if (!xpointer || strncmp(xpointer, kPrefix, sizeof(kPrefix) - 1) != 0) return -1;
  const char* p = xpointer + sizeof(kPrefix) - 1;
  if (*p == '/' || *p == '\0') return 0;  // A document of one fragment writes no index.
  if (*p != '[') return -1;
  int32_t value = 0;
  for (++p; *p >= '0' && *p <= '9'; ++p) {
    if (value > 100000) return -1;
    value = value * 10 + (*p - '0');
  }
  return *p == ']' && value >= 1 ? value - 1 : -1;
}

// ---------------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------------

InboundStore::InboundStore(IFs& fs) : fs_(fs), index_(fs, kIndexPath) {}

InboundStore::~InboundStore() { delete[] buffer_; }

bool InboundStore::beginRun(const BookId& book, const uint16_t epoch) {
  if (!buffer_) buffer_ = new (std::nothrow) uint8_t[kMaxRecordBytes];
  if (!buffer_) return false;
  book_ = book;
  epoch_ = epoch;
  deletedSome_ = false;
  wrote_ = false;
  indexed_ = false;
  char path[kPathCap];
  logPath(book, path);
  if (!recoverAside(fs_, path, epoch, buffer_)) return false;
  Fold fold;
  if (!foldLog(fs_, path, epoch, buffer_, nullptr, 0, fold)) return false;
  exists_ = fold.usable;
  damaged_ = fold.damaged;
  records_ = fold.records;
  bytes_ = fold.bytes;
  watermark_ = fold.watermark;
  open_ = true;
  return true;
}

bool InboundStore::ensureLog() {
  if (exists_) return true;
  char path[kPathCap];
  logPath(book_, path);
  char dir[kPathCap];
  shardDir(path, dir);
  {
    File f(fs_, path, IFs::Mode::WriteTrunc);
    if (!f) {
      f.close();
      if (!fs_.mkdir(kInboundDir) || !fs_.mkdir(dir)) return false;
      File again(fs_, path, IFs::Mode::WriteTrunc);
      if (!again || !writeLogHeader(again, epoch_, 0, watermark_) || !again.flush()) return false;
    } else if (!writeLogHeader(f, epoch_, 0, watermark_) || !f.flush()) {
      return false;
    }
  }
  exists_ = true;
  damaged_ = false;
  records_ = 0;
  bytes_ = kLogHeaderBytes;
  return true;
}

bool InboundStore::append(const InboundHighlight& h) {
  const size_t len = encodeHighlight(h, buffer_);
  if (len == 0) return true;  // Cannot happen: every field is bounded by its buffer.
  const uint32_t framed = static_cast<uint32_t>(kFrameHeaderBytes + len);
  if (damaged_ || records_ + 1u > kInboundMaxRecords || bytes_ + framed > kInboundMaxBookBytes) {
    if (!compact()) return false;
    // The buffer was used for the rewrite. Only a log that came out empty goes away.
    if (encodeHighlight(h, buffer_) != len) return false;
    if (!exists_ && !ensureLog()) return false;
  }
  char path[kPathCap];
  logPath(book_, path);
  File f(fs_, path, IFs::Mode::Append);
  uint8_t header[kFrameHeaderBytes];
  frameHeader(buffer_, len, header);
  if (!f || !f.writeAll(header, sizeof(header)) || !f.writeAll(buffer_, len) || !f.flush()) return false;
  bytes_ += framed;
  ++records_;
  wrote_ = true;
  return true;
}

bool InboundStore::apply(const BookId& book, const uint16_t epoch, const InboundHighlight& h) {
  if (!open_ || epoch_ != epoch || memcmp(book_.md5, book.md5, 16) != 0) {
    if (!finish() || !beginRun(book, epoch)) return false;
  }
  if (h.serverSeq <= watermark_) return true;
  if (h.deleted && !exists_) {
    // Nothing to take away, and nothing to keep: a replay comes back with the delete after it.
    watermark_ = h.serverSeq;
    return true;
  }
  // Named before it exists: a crash leaves a name with no file, never a file nobody can find to count
  // or to delete. Also what makes this the book used last.
  if (!indexed_ && !saveIndexEntry(true, false)) return false;
  indexed_ = true;
  if (!ensureLog() || !append(h)) return false;
  watermark_ = h.serverSeq;
  if (h.deleted) deletedSome_ = true;
  return true;
}

bool InboundStore::finish() {
  if (!open_) return true;
  // What was deleted leaves the card before the cursor says it was dealt with, not at some later
  // compaction.
  if (deletedSome_ && exists_ && !compact()) return false;
  if (wrote_ || deletedSome_) {
    if (!saveIndexEntry(exists_, true)) return false;
  }
  open_ = false;
  return true;
}

bool InboundStore::compact() {
  auto* rows = new (std::nothrow) Row[kInboundMaxRecords];
  if (!rows) return false;
  char path[kPathCap];
  logPath(book_, path);
  char aside[kPathCap + 3];
  asidePath(path, aside);
  Fold fold;
  bool ok = foldLog(fs_, path, epoch_, buffer_, rows, kInboundMaxRecords, fold);
  if (ok) {
    sortByOffset(rows, fold.count);
    // From the newest back, up to 3/4 of each bound: room for the next few without coming back here.
    const uint32_t byteBudget = kInboundMaxBookBytes / 4 * 3;
    const uint16_t rowBudget = kInboundMaxRecords / 4 * 3;
    uint32_t bytes = kLogHeaderBytes + kFrameHeaderBytes + 1;
    uint16_t kept = 0;
    uint16_t lost = 0;
    uint16_t first = fold.count;
    for (uint16_t i = fold.count; i-- > 0;) {
      if (rows[i].length & kRowDeleted) continue;
      const uint32_t framed = kFrameHeaderBytes + rows[i].length;
      if (kept >= rowBudget || bytes + framed > byteBudget) {
        ++lost;
        rows[i].length |= kRowDeleted;
        continue;
      }
      bytes += framed;
      ++kept;
      first = i;
    }
    const uint32_t dropped = fold.dropped + static_cast<uint32_t>(lost);
    const uint16_t droppedNow = dropped > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(dropped);
    {
      File in(fs_, path, IFs::Mode::Read);
      File out(fs_, aside, IFs::Mode::WriteTrunc);
      ok = in && out && writeLogHeader(out, epoch_, droppedNow, watermark_);
      for (uint16_t i = first; ok && i < fold.count; ++i) {
        if (rows[i].length & kRowDeleted) continue;
        uint8_t header[kFrameHeaderBytes];
        const uint16_t length = rows[i].length;
        ok = in.seek(rows[i].offset) && in.readExact(header, sizeof(header)) && in.readExact(buffer_, length) &&
             out.writeAll(header, sizeof(header)) && out.writeAll(buffer_, length);
      }
      if (ok) {
        const uint8_t seal = kKindSeal;
        uint8_t header[kFrameHeaderBytes];
        frameHeader(&seal, 1, header);
        ok = out.writeAll(header, sizeof(header)) && out.writeAll(&seal, 1) && out.flush();
      }
    }
    if (ok) {
      ok = fs_.remove(path) && fs_.rename(aside, path);
    } else {
      fs_.remove(aside);
    }
    if (ok) {
      exists_ = true;
      damaged_ = false;
      records_ = kept;
      bytes_ = bytes;
    }
    if (ok && kept == 0 && droppedNow == 0) {
      // Nothing left to list and nothing to say about it.
      ok = fs_.remove(path);
      if (ok) {
        exists_ = false;
        bytes_ = 0;
        records_ = 0;
      }
    }
  }
  delete[] rows;
  return ok;
}

bool InboundStore::saveIndexEntry(const bool present, const bool bytesOnly) {
  auto* buffer = new (std::nothrow) uint8_t[kIndexBytes];
  if (!buffer) return false;
  uint16_t epoch = 0;
  uint16_t count = 0;
  bool ok = readIndex(index_, buffer, epoch, count);
  bool changed = false;
  if (ok && count > 0 && epoch != epoch_) {
    // Another account's books, from a change that did not get to purge them.
    for (uint16_t i = 0; i < count; ++i) removeBook(fs_, buffer + kIndexHeaderBytes + i * kIndexEntryBytes);
    count = 0;
    changed = true;
  }
  uint8_t* entries = buffer + kIndexHeaderBytes;
  int found = -1;
  if (ok) {
    for (uint16_t i = 0; i < count && found < 0; ++i) {
      if (memcmp(entries + i * kIndexEntryBytes, book_.md5, 16) == 0) found = i;
    }
  }
  if (ok && !present) {
    if (found >= 0) {
      memmove(entries + found * kIndexEntryBytes, entries + (found + 1) * kIndexEntryBytes,
              (count - found - 1u) * kIndexEntryBytes);
      --count;
      changed = true;
    }
  } else if (ok && (!bytesOnly || found < 0)) {
    // Used now: to the end of the line.
    if (found >= 0 && found != count - 1) {
      uint8_t held[kIndexEntryBytes];
      memcpy(held, entries + found * kIndexEntryBytes, kIndexEntryBytes);
      memmove(entries + found * kIndexEntryBytes, entries + (found + 1) * kIndexEntryBytes,
              (count - found - 1u) * kIndexEntryBytes);
      memcpy(entries + (count - 1u) * kIndexEntryBytes, held, kIndexEntryBytes);
      changed = true;
    } else if (found < 0) {
      memcpy(entries + count * kIndexEntryBytes, book_.md5, 16);
      setEntryBytes(entries + count * kIndexEntryBytes, bytes_);
      ++count;
      changed = true;
    }
    found = count - 1;
  }
  if (ok && present && found >= 0 && entryBytes(entries + found * kIndexEntryBytes) != bytes_) {
    setEntryBytes(entries + found * kIndexEntryBytes, bytes_);
    changed = true;
  }
  if (ok && present) {
    // The bounds of the whole card. The current book is the last of the line, so it stays.
    uint64_t total = 0;
    for (uint16_t i = 0; i < count; ++i) total += entryBytes(entries + i * kIndexEntryBytes);
    uint16_t gone = 0;
    while (count - gone > 1 && (count - gone > kInboundMaxBooks || total > kInboundMaxTotalBytes)) {
      const uint8_t* entry = entries + gone * kIndexEntryBytes;
      if (memcmp(entry, book_.md5, 16) == 0) break;  // Only when bytesOnly left it where it was.
      total -= entryBytes(entry);
      removeBook(fs_, entry);
      ++gone;
    }
    if (gone) {
      memmove(entries, entries + gone * kIndexEntryBytes, (count - gone) * kIndexEntryBytes);
      count = static_cast<uint16_t>(count - gone);
      changed = true;
    }
  }
  if (ok && (changed || epoch != epoch_)) {
    ByteWriter w(buffer, kIndexHeaderBytes);
    w.u8(kIndexVersion);
    w.u16(epoch_);
    w.u16(count);
    ok = index_.save(buffer, kIndexHeaderBytes + count * kIndexEntryBytes);
    if (!ok) {
      ok = fs_.mkdir(kInboundDir) && index_.save(buffer, kIndexHeaderBytes + count * kIndexEntryBytes);
    }
  }
  delete[] buffer;
  return ok;
}

void InboundStore::purge(IFs& fs) {
  AtomicFile index(fs, kIndexPath);
  auto* buffer = new (std::nothrow) uint8_t[kIndexBytes];
  if (!buffer) return;  // The next write that finds the index of another epoch does it.
  uint16_t epoch = 0;
  uint16_t count = 0;
  if (readIndex(index, buffer, epoch, count)) {
    for (uint16_t i = 0; i < count; ++i) removeBook(fs, buffer + kIndexHeaderBytes + i * kIndexEntryBytes);
    index.removeAll();
  }
  delete[] buffer;
}

// ---------------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------------

namespace {

class LiveVisitor {
 public:
  virtual ~LiveVisitor() = default;
  // How many there can be. False: no memory for them.
  virtual bool prepare(uint16_t) { return true; }
  // False stops the walk.
  virtual bool onLive(uint32_t offset, const InboundHighlight& highlight) = 0;
};

bool visitLive(IFs& fs, const BookId& book, const uint16_t epoch, LiveVisitor& visitor, bool* partial) {
  if (!InboundList::present(fs, book)) return true;  // Most books: no allocation at all.
  char path[kPathCap];
  logPath(book, path);

  auto* buffer = new (std::nothrow) uint8_t[kMaxRecordBytes];
  if (!buffer) return false;
  // Sized from the log, not the bound: reading it first costs nothing more than this pass.
  Fold totals;
  bool ok = recoverAside(fs, path, epoch, buffer) && foldLog(fs, path, epoch, buffer, nullptr, 0, totals);
  if (!ok || !totals.usable || totals.records == 0) {
    delete[] buffer;
    return ok;
  }
  const uint16_t cap = totals.records < kInboundMaxRecords ? totals.records : kInboundMaxRecords;
  auto* rows = new (std::nothrow) Row[cap];
  auto* highlight = new (std::nothrow) InboundHighlight;
  ok = rows && highlight;
  Fold fold;
  if (ok) ok = foldLog(fs, path, epoch, buffer, rows, cap, fold);
  if (ok) {
    if (partial) *partial = fold.dropped > 0;
    sortByOffset(rows, fold.count);
    uint16_t live = 0;
    for (uint16_t i = 0; i < fold.count; ++i) {
      if (!(rows[i].length & kRowDeleted)) ++live;
    }
    ok = visitor.prepare(live);
    File f(fs, path, IFs::Mode::Read);
    ok = ok && static_cast<bool>(f);
    for (uint16_t i = 0; ok && i < fold.count; ++i) {
      if (rows[i].length & kRowDeleted) continue;
      const uint16_t length = rows[i].length;
      if (!f.seek(rows[i].offset + kFrameHeaderBytes) || !f.readExact(buffer, length)) {
        ok = false;
        break;
      }
      if (!decodeHighlight(buffer, length, *highlight)) continue;
      if (!visitor.onLive(rows[i].offset, *highlight)) break;
    }
  }
  delete highlight;
  delete[] rows;
  delete[] buffer;
  return ok;
}

}  // namespace

InboundList::~InboundList() { delete[] items_; }

bool InboundList::present(IFs& fs, const BookId& book) {
  char path[kPathCap];
  logPath(book, path);
  if (fs.exists(path)) return true;
  char aside[kPathCap + 3];
  asidePath(path, aside);
  return fs.exists(aside);
}

bool InboundList::load(const BookId& book, const uint16_t epoch, IOwnPassages* own) {
  delete[] items_;
  items_ = nullptr;
  count_ = 0;
  partial_ = false;
  book_ = book;

  class Collect final : public LiveVisitor {
   public:
    explicit Collect(IOwnPassages* own) : own_(own) {}
    ~Collect() override { delete[] items_; }
    bool prepare(const uint16_t live) override {
      items_ = new (std::nothrow) Item[live ? live : 1];
      cap_ = live;
      return items_ != nullptr;
    }
    Item* take() {
      Item* items = items_;
      items_ = nullptr;
      return items;
    }
    bool onLive(const uint32_t offset, const InboundHighlight& h) override {
      const int32_t spine = spineOfXpointer(h.xpointer);
      if (own_ && own_->isOwn(h.passage, spine)) return true;
      const auto passage = static_cast<uint32_t>(h.passage);
      Item item{offset, h.hasPct ? h.pctMicro : UINT32_MAX, passage, spine};
      // Two other devices with the same passage: the newest says it, in its place.
      for (uint16_t i = 0; i < count; ++i) {
        if (items_[i].passage == passage && (items_[i].spine == spine || items_[i].spine < 0 || spine < 0)) {
          items_[i] = item;
          return true;
        }
      }
      if (count < cap_) items_[count++] = item;
      return true;
    }
    uint16_t count = 0;

   private:
    Item* items_ = nullptr;
    uint16_t cap_ = 0;
    IOwnPassages* own_;
  };

  Collect collect(own);
  if (!visitLive(fs_, book, epoch, collect, &partial_)) return false;
  items_ = collect.take();
  count_ = collect.count;
  // Book order. What cannot be placed goes after what can, as it came.
  for (uint16_t i = 1; i < count_; ++i) {
    const Item key = items_[i];
    uint16_t j = i;
    while (j > 0 && items_[j - 1].pctMicro > key.pctMicro) {
      items_[j] = items_[j - 1];
      --j;
    }
    items_[j] = key;
  }
  return true;
}

bool InboundList::read(const uint16_t index, InboundHighlight& out) {
  if (!items_ || index >= count_) return false;
  auto* buffer = new (std::nothrow) uint8_t[kMaxRecordBytes];
  if (!buffer) return false;
  char path[kPathCap];
  logPath(book_, path);
  bool ok = false;
  {
    File f(fs_, path, IFs::Mode::Read);
    uint8_t header[kFrameHeaderBytes];
    if (f && f.seek(items_[index].offset) && f.readExact(header, sizeof(header))) {
      ByteReader r(header + 2, sizeof(header) - 2);
      const uint16_t len = r.u16();
      const uint32_t crc = r.u32();
      ok = len <= kMaxRecordBytes && f.readExact(buffer, len) && crc32(buffer, len) == crc &&
           decodeHighlight(buffer, len, out);
    }
  }
  delete[] buffer;
  return ok;
}

bool InboundList::hasAny(IFs& fs, const BookId& book, const uint16_t epoch, IOwnPassages* own) {
  class First final : public LiveVisitor {
   public:
    explicit First(IOwnPassages* own) : own_(own) {}
    bool onLive(uint32_t, const InboundHighlight& h) override {
      found = !own_ || !own_->isOwn(h.passage, spineOfXpointer(h.xpointer));
      return !found;
    }
    bool found = false;

   private:
    IOwnPassages* own_;
  };
  First first(own);
  return visitLive(fs, book, epoch, first, nullptr) && first.found;
}

}  // namespace borges
