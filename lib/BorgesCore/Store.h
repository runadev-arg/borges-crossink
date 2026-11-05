#pragma once

// Everything that survives a reboot besides the outbox:
//
//   /.crosspoint/borges/cred.{a,b}          Credentials (token obfuscated with the chip key)
//   /.crosspoint/borges/state.{a,b}         Cursor, holds, backoff, diagnostics
//   /.crosspoint/borges/books/<md5>.{a,b}   Per-book ledger: remote head, choices, undo
//   /.crosspoint/borges/ann/<md5>.{a,b}     Which clippings this device already published
//   /.crosspoint/borges/inbox/<md5>[.old]   Annotations from other devices, read-only list
//   /.crosspoint/borges/spool/resp.json     Last response
//
// Ledgers belong to the account: state.accountEpoch moves on an account change and a
// ledger from an older epoch reads as blank, so no directory walk is ever needed.

#include <cstddef>
#include <cstdint>

#include "AtomicFile.h"
#include "Outbox.h"
#include "Ports.h"
#include "Types.h"

namespace borges {

constexpr const char* kBorgesDir = "/.crosspoint/borges";
constexpr const char* kOutboxDir = "/.crosspoint/borges/outbox";
constexpr const char* kSpoolPath = "/.crosspoint/borges/spool/resp.json";
constexpr const char* kDefaultBaseUrl = "https://borges.runadev.com";
constexpr const char* kLegacyCredentialPath = "/.borges/borges.json";
constexpr const char* kLegacyCredentialPathOld = "/.borges/toto.json";

struct PendingPairing {
  char requestId[40] = {0};
  char nonce[44] = {0};
  char userCode[12] = {0};
  char verificationUrl[96] = {0};
  uint32_t intervalSec = 5;
  int64_t expiresAtEpoch = 0;

  bool active() const { return requestId[0] != '\0' && nonce[0] != '\0'; }
};

struct Credentials {
  char baseUrl[96] = {0};
  char deviceId[40] = {0};
  char token[128] = {0};
  char accountId[40] = {0};
  int64_t expiresAtEpoch = 0;   // 0 = unknown. Pairing credentials may have no expiry.
  int64_t renewAfterEpoch = 0;  // 0 = unknown.
  char serverIp[16] = {0};      // Last IPv4 that worked, for the DNS chain.
  char serverIpHost[64] = {0};  // The host it belongs to.
  PendingPairing pairing;

  bool paired() const { return deviceId[0] != '\0' && token[0] != '\0'; }
};

class HoldSet : public IHoldFilter {
 public:
  static constexpr uint8_t kMax = 32;
  bool isHeld(uint32_t bookTag) const override;
  bool contains(const uint8_t md5[16]) const;
  // True if the set changed. A full set evicts its oldest hold and counts it.
  bool add(const uint8_t md5[16]);
  bool remove(const uint8_t md5[16]);
  uint8_t size() const { return count_; }
  const uint8_t* at(uint8_t i) const { return items_[i]; }
  uint32_t evicted() const { return evicted_; }
  void clear() { count_ = 0; }

 private:
  friend class Store;
  uint8_t items_[kMax][16];
  uint8_t count_ = 0;
  uint32_t evicted_ = 0;
};

struct SyncState {
  uint64_t cursor = 0;           // Everything up to here is applied and persisted.
  uint64_t committedCursor = 0;  // What the server has been told about it.
  uint16_t accountEpoch = 0;
  uint8_t failures = 0;
  bool needsRelink = false;      // 401/403: stop automatic sessions, keep the queue.
  uint32_t capsClientHash = 0;   // Non-zero: capabilities verified for this client_version.
  int64_t nextAttemptEpoch = 0;
  int64_t anchorEpoch = 0;       // Last server_time seen.
  int64_t lastSuccessEpoch = 0;
  uint32_t skippedEvents = 0;    // Pulled events skipped for exceeding the parse budget.
  uint64_t lastSkippedSeq = 0;
  uint32_t rejectedEvents = 0;   // Events the server rejected.
  char lastRejection[40] = {0};
  Diagnostics diag;
  HoldSet holds;
};

enum class SuggestionAction : uint8_t { None = 0, Accept = 1, Dismiss = 2 };

struct RemoteHead {
  bool valid = false;
  uint64_t serverSeq = 0;
  Position position;
  TimePrecision precision = TimePrecision::Unknown;
  char occurredAt[32] = {0};
  char deviceName[48] = {0};
};

struct BookLedger {
  static constexpr uint8_t kMaxChoices = 40;

  BookId id;
  uint64_t inboundSeq = 0;        // Newest inbound annotation already stored.
  RemoteHead head;                // Latest progress of another device seen in pulls.
  bool hasUndo = false;
  Position undo;
  bool hasFinalized = false;      // lastFinalized and lastTurnEpoch are meaningful.
  Position lastFinalized;
  int64_t lastTurnEpoch = 0;
  char suggestionId[40] = {0};    // Open suggestion for this book.
  SuggestionAction suggestionAction = SuggestionAction::None;  // Decided, not yet told to the server.
  uint8_t choiceCount = 0;
  uint64_t choices[kMaxChoices] = {0};  // Fingerprints of answered events, oldest first.

  void remember(uint64_t fingerprint);
  bool hasChoice(uint64_t fingerprint) const;
};

struct AnnotationRecord {
  uint64_t key;
  uint8_t syncId[16];
  uint8_t bookmark;
};

// One annotation received from another device.
struct InboundAnnotation {
  uint64_t serverSeq = 0;
  bool deleted = false;
  bool textTruncated = false;
  int32_t page = -1;
  char syncId[40] = {0};
  char chapter[96] = {0};
  char note[256] = {0};
  char text[1024] = {0};
};

class IInboundVisitor {
 public:
  virtual ~IInboundVisitor() = default;
  virtual void onAnnotation(const InboundAnnotation& annotation) = 0;
};

class Store {
 public:
  static constexpr uint16_t kMaxAnnotations = 512;
  static constexpr uint32_t kInboxRotateBytes = 32 * 1024;

  // key: the chip's MAC, the same key the previous firmware obfuscated secrets with.
  Store(IFs& fs, const uint8_t* key, size_t keyLen);

  // Creates directories and loads everything. Imports the previous firmware's
  // credential once, silently, when there is none. False only on a hard IO error.
  bool begin();

  Credentials& creds() { return creds_; }
  const Credentials& creds() const { return creds_; }
  bool saveCredentials();

  SyncState& state() { return state_; }
  const SyncState& state() const { return state_; }
  bool saveState();

  // A pairing resolved to this account. A different account than before moves the
  // epoch and empties the queue and the cursor: its data belongs to someone else.
  bool adoptAccount(const char* accountId, Outbox& outbox);
  // Forgets the credential (and the account's cursor), keeping the sequence counter.
  bool forgetCredential();

  // A blank ledger with its id set when there is none: only IO is a failure.
  bool loadBook(const BookId& id, BookLedger& out);
  bool saveBook(const BookLedger& ledger);

  // Allocates exactly what is needed. Returns false on IO error or lack of memory.
  // *records must be released with delete[].
  bool loadAnnotations(const BookId& id, AnnotationRecord** records, uint16_t* count);
  bool saveAnnotations(const BookId& id, const AnnotationRecord* records, uint16_t count);

  bool appendInbound(const BookId& id, const InboundAnnotation& annotation);
  // Oldest first. Corrupt bytes are skipped.
  bool forEachInbound(const BookId& id, InboundAnnotation& scratch, IInboundVisitor& visitor);

  IFs& fs() { return fs_; }

 private:
  void bookPath(const char* subdir, const BookId& id, char* out) const;
  bool importLegacy();
  void obfuscate(char* text, size_t len) const;
  bool readInboxFile(const char* path, InboundAnnotation& scratch, IInboundVisitor& visitor, uint64_t& lastSeq);

  IFs& fs_;
  uint8_t key_[8];
  uint8_t keyLen_;
  AtomicFile credFile_;
  AtomicFile stateFile_;
  Credentials creds_;
  SyncState state_;
};

}  // namespace borges
