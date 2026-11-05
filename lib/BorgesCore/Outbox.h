#pragma once

// Durable queue of events waiting for an ACK.
//
// Layout (all under one directory):
//   outbox.<gen>.log   Append-only CRC records. <gen> names the live generation.
//   outbox.meta.{a,b}  AtomicFile: live generation + sequence floor.
//   outbox.bad         Quarantined bytes, capped. Never read back.
//
// Records:  magic u16 | type u8 | check u8 | len u32 | crc u32 | payload
//   EVENT     id, seq, book, kind, attempted, replaces, JSON body (immutable)
//   ATTEMPT   offsets of events selected for transport (they become immutable)
//   ACK       offsets of events the server answered; they leave the queue
//   RENUMBER  (offset, seq) pairs, the persisted form of a sequence repair
//
// Replaying the log rebuilds a small RAM index of live events. Compaction writes a
// new generation and commits it by saving the meta record, so a crash leaves either
// the old generation or the new one and never a mix: rename is never used.
//
// A record with a bad CRC is copied to outbox.bad and skipped; a torn tail is the
// same case. depth() counts exactly the valid events nextBatch can return.

#include <cstddef>
#include <cstdint>

#include "AtomicFile.h"
#include "Ports.h"

namespace borges {

class JsonWriter;

enum class EventKind : uint8_t { Progress = 1, Annotation = 2, Session = 3, PageStats = 4 };

class IHoldFilter {
 public:
  virtual ~IHoldFilter() = default;
  virtual bool isHeld(uint32_t bookTag) const = 0;
};

constexpr uint32_t bookTagOf(const uint8_t md5[16]) {
  return static_cast<uint32_t>(md5[0]) | static_cast<uint32_t>(md5[1]) << 8 | static_cast<uint32_t>(md5[2]) << 16 |
         static_cast<uint32_t>(md5[3]) << 24;
}

struct OutboxEntry {
  uint32_t offset;
  uint32_t payloadLen;
  uint64_t seq;
  uint32_t bookTag;
  uint8_t kind;
  uint8_t attempted;
};

constexpr uint8_t kMaxBatchEvents = 20;

struct BatchItem {
  uint8_t id[16];
  uint64_t seq;
  uint32_t offset;
};

struct Batch {
  BatchItem items[kMaxBatchEvents];
  uint8_t count = 0;
  size_t wireBytes = 0;  // Upper bound of the serialized events.
};

class Outbox {
 public:
  static constexpr uint16_t kMaxLiveEvents = 128;
  static constexpr uint32_t kMaxRecordBytes = 48 * 1024;
  static constexpr uint32_t kMaxBadBytes = 64 * 1024;
  static constexpr uint32_t kCompactDeadBytes = 4 * 1024;

  enum class AppendResult : uint8_t { Ok, Full, TooLarge, IoError, NotOpen };

  Outbox(IFs& fs, IRandom& random, const char* dir);
  ~Outbox();
  Outbox(const Outbox&) = delete;
  Outbox& operator=(const Outbox&) = delete;

  // Replays the log and quarantines anything corrupt. False on IO error or lack of
  // memory: nothing was changed and the queue is simply unavailable for now.
  bool open();
  void close();
  bool isOpen() const { return open_; }

  // Durable once it returns Ok. With replaceUnattempted, an existing progress event
  // of the same book that was never attempted is replaced in the same atomic record.
  AppendResult append(EventKind kind, const uint8_t md5[16], const char* body, size_t bodyLen, bool replaceUnattempted,
                      uint8_t outId[16], uint64_t* outSeq);

  // Live valid events. queue_depth on the wire.
  size_t depth() const { return count_; }
  // Live events that are not held back right now.
  size_t sendableDepth(const IHoldFilter* holds) const;
  bool hasEventFor(uint32_t bookTag, EventKind kind) const;
  uint64_t nextSequence() const { return nextSeq_; }
  uint32_t generation() const { return gen_; }
  uint32_t logBytes() const { return logSize_; }
  uint32_t quarantinedRecords() const { return quarantined_; }

  // Selects the oldest eligible events: at most maxEvents, and while the serialized
  // size stays within maxBytes, but always at least one. skip is optional, sorted or
  // not: offsets to pass over for this session.
  void selectBatch(size_t maxEvents, size_t maxBytes, const IHoldFilter* holds, const uint32_t* skip, size_t skipCount,
                   Batch& out) const;

  // Persists "these were attempted" before any network IO.
  bool markAttempted(const Batch& batch);
  enum class EnvelopeResult : uint8_t {
    Ok,
    Quarantined,  // The record no longer verifies. It left the queue: select again.
    IoError,      // Could not read it. Says nothing about the record, which stays.
  };
  // Serializes one envelope.
  EnvelopeResult writeEnvelope(BatchItem& item, JsonWriter& out);

  // One atomic record: every listed event leaves the queue together.
  bool ack(const uint32_t* offsets, size_t count);
  // Copies the record to outbox.bad, then acks it.
  bool quarantine(uint32_t offset);

  // Sequence repair: proven events and never-attempted ones get new numbers above
  // the local maximum and the server's floor, in log order, in one atomic record.
  // Ids and bodies do not change.
  bool renumber(const uint32_t* provenOffsets, size_t provenCount, uint64_t floor);

  bool compactIfNeeded();
  bool compact();

  // Account switch: drops all events, keeps the sequence high-water mark.
  bool reset();

  const OutboxEntry* entryAt(size_t index) const { return index < count_ ? &entries_[index] : nullptr; }

 private:
  bool scan();
  bool addEntry(const OutboxEntry& entry);
  void removeEntry(uint32_t offset);
  OutboxEntry* findEntry(uint32_t offset);
  bool appendOffsets(uint8_t type, const uint32_t* offsets, size_t count);
  void quarantineRange(File& source, uint32_t start, uint32_t len);
  void logPath(uint32_t gen, char* out) const;
  enum class Verify : uint8_t { Valid, Invalid, IoError };
  Verify verifyRecord(File& f, uint32_t pos, uint32_t fileSize, uint8_t& type, uint32_t& len);
  bool saveMeta(uint32_t gen);

  IFs& fs_;
  IRandom& random_;
  char dir_[AtomicFile::kMaxPath];
  AtomicFile meta_;
  OutboxEntry* entries_ = nullptr;
  uint16_t count_ = 0;
  uint16_t capacity_ = 0;
  bool open_ = false;
  bool needsCompaction_ = false;
  uint32_t gen_ = 1;
  uint32_t logSize_ = 0;
  uint64_t nextSeq_ = 1;
  uint64_t floor_ = 1;
  uint32_t quarantined_ = 0;
};

}  // namespace borges
