#pragma once

// Highlights made on other devices or on the web, kept per book for a read-only list.
//
//   /.crosspoint/borges/inbound/<xx>/<rest of md5>    One append-only log per book
//   /.crosspoint/borges/inbound/<xx>/<rest of md5>.n  Its compaction, on the way in
//   /.crosspoint/borges/inbound/index.{a,b}           Which books have one, oldest use first
//
// Log: a header (epoch, watermark, how many were dropped), then CRC-framed records. An update
// or a delete is one more record. A crash can only tear the tail, and the cursor has not moved
// past what was torn, so the server sends it again.
//
// Bounds, all of them always true on the card:
//   a log holds at most kInboundMaxRecords records and kInboundMaxBookBytes bytes. Reaching
//   either compacts it, dropping the oldest, down to 3/4 of each.
//   the index knows at most kInboundMaxBooks books, totalling kInboundMaxTotalBytes. Past that
//   the book used longest ago goes whole.
//
// Memory: nothing resident. Pulling holds a record and its encoding (under 2.5 KB) for the
// session, and 4 KB for the moment a compaction or an index update takes. The list holds 16 B
// per highlight of the book while it is open, and 4 KB more at most while it opens.
//
// Compaction never renames over a file, which FAT cannot do atomically: the new log is written
// aside and ends in a seal record. A sealed .n found at any open wins, and an unsealed one is
// what a crash left and goes.

#include <cstddef>
#include <cstdint>

#include "AtomicFile.h"
#include "Ports.h"
#include "Types.h"

namespace borges {

constexpr uint16_t kInboundMaxRecords = 256;
constexpr uint32_t kInboundMaxBookBytes = 64 * 1024;
constexpr uint32_t kInboundMaxTotalBytes = 4 * 1024 * 1024;
constexpr uint16_t kInboundMaxBooks = 200;
constexpr size_t kInboundTextMax = 480;  // Far more than a row shows. Longer is cut and flagged.
constexpr size_t kInboundChapterMax = 96;
constexpr size_t kInboundDeviceMax = 48;

constexpr const char* kInboundDir = "/.crosspoint/borges/inbound";

struct InboundHighlight {
  uint8_t syncId[16] = {0};
  uint64_t serverSeq = 0;
  uint32_t revision = 0;  // 0: the server did not say.
  bool deleted = false;
  bool truncated = false;
  bool hasPct = false;
  uint32_t pctMicro = 0;
  int64_t timeEpoch = 0;  // 0: when it was made is not known.
  uint64_t passage = 0;   // passageHash(text).
  char xpointer[kMaxXpointer] = {0};
  char chapter[kInboundChapterMax] = {0};
  char device[kInboundDeviceMax] = {0};
  char text[kInboundTextMax] = {0};
};

// The same passage whatever the device did to its white space: the same rule as
// annotationadapter.lua:comparableText, over the first 200 bytes that are left, which is
// what fits for certain in a stored record. 0 for a text with nothing in it.
uint64_t passageHash(const char* text);
// The spine an xpointer names, as the mapper reads it. -1: none.
int32_t spineOfXpointer(const char* xpointer);

// What this device highlighted itself. A passage on both sides shows once, as its own.
class IOwnPassages {
 public:
  virtual ~IOwnPassages() = default;
  // spine -1: the highlight does not say where it is, so the text alone decides.
  virtual bool isOwn(uint64_t passage, int32_t spine) = 0;
};

// The writing side, for a session. Every record is durable when apply() returns, and nothing
// but the index is pending after that, so the cursor may move once finish() returned.
class InboundStore {
 public:
  explicit InboundStore(IFs& fs);
  ~InboundStore();
  InboundStore(const InboundStore&) = delete;
  InboundStore& operator=(const InboundStore&) = delete;

  // Idempotent per book: what the log has seen goes no further. False only on IO or lack of memory.
  bool apply(const BookId& book, uint16_t epoch, const InboundHighlight& highlight);
  bool finish();

  // Another account took the card. By the index: there is no directory walk on this file system.
  static void purge(IFs& fs);

 private:
  bool beginRun(const BookId& book, uint16_t epoch);
  bool ensureLog();
  bool append(const InboundHighlight& highlight);
  bool compact();
  bool saveIndexEntry(bool present, bool bytesOnly);

  IFs& fs_;
  AtomicFile index_;
  uint8_t* buffer_ = nullptr;  // One encoded record.

  bool open_ = false;
  BookId book_;
  uint16_t epoch_ = 0;
  bool exists_ = false;
  bool damaged_ = false;
  bool deletedSome_ = false;
  bool wrote_ = false;
  bool indexed_ = false;
  uint16_t records_ = 0;
  uint32_t bytes_ = 0;
  uint64_t watermark_ = 0;
};

// Every highlight a log says is live, in the order they arrived.
class InboundVisitor {
 public:
  virtual ~InboundVisitor() = default;
  // How many there can be. False: no memory for them.
  virtual bool prepare(uint16_t) { return true; }
  // False stops the walk.
  virtual bool onLive(uint32_t offset, const InboundHighlight& highlight) = 0;
};

// The reading side. Open for as long as a screen shows it.
class InboundList {
 public:
  explicit InboundList(IFs& fs) : fs_(fs) {}
  ~InboundList();
  InboundList(const InboundList&) = delete;
  InboundList& operator=(const InboundList&) = delete;

  // In book order, one row per passage, without what the device highlighted itself. own may be null.
  // False only on IO or lack of memory. A log of another epoch reads as empty.
  bool load(const BookId& book, uint16_t epoch, IOwnPassages* own);
  uint16_t count() const { return count_; }
  // Older highlights of this book were dropped to stay within the bounds.
  bool partial() const { return partial_; }
  bool read(uint16_t index, InboundHighlight& out);

  // Without a list: stops at the first one that would be a row.
  static bool hasAny(IFs& fs, const BookId& book, uint16_t epoch, IOwnPassages* own);
  // Two lookups, no memory. False: no book on this card has one, not even whose it is needs asking.
  static bool anyOnCard(IFs& fs);
  // One or two lookups, no memory. False: this book has none, and there is no need to ask more.
  static bool present(IFs& fs, const BookId& book);
  // Holds up to 4 KB and a record while it runs. True with nothing visited when the book has no log.
  static bool visit(IFs& fs, const BookId& book, uint16_t epoch, InboundVisitor& visitor);
  // Changes whenever what the log holds changes: an append moves its size, a compaction its header.
  // Two small reads and no allocation, unless a crash left a compaction to finish. False: no usable
  // log of this epoch.
  static bool identity(IFs& fs, const BookId& book, uint16_t epoch, uint32_t& out);

 private:
  struct Item {
    uint32_t offset;
    uint32_t pctMicro;  // UINT32_MAX: last.
    uint32_t passage;
    int32_t spine;
  };

  IFs& fs_;
  BookId book_;
  Item* items_ = nullptr;
  uint16_t count_ = 0;
  bool partial_ = false;
};

}  // namespace borges
