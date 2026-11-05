#pragma once

// Everything that survives a reboot besides the outbox:
//
//   /.crosspoint/borges/cred.{a,b}          Credentials (token and nonce sealed: see ISecretBox)
//   /.crosspoint/borges/state.{a,b}         Cursor, holds, backoff, diagnostics
//   /.crosspoint/borges/books/<xx>/<rest of md5>.{a,b}  Per-book ledger: head, choices, undo
//   /.crosspoint/borges/ann/<xx>/<rest of md5>.{a,b}    Clippings this device already published
//
// Sharded by the first hex byte: with 500 books a flat directory holds 1000 long names,
// FAT looks them up one by one, and a page of 20 books would spend most of a 12 s open
// budget on directory scans.
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
  // Always the compiled-in host when begin() returns: nothing on the card can point this
  // device, and its token, at another server. Only test builds change it, in RAM.
  char baseUrl[96] = {0};
  char deviceId[40] = {0};
  char token[128] = {0};
  char accountId[40] = {0};     // Empty if the server did not say, or for an imported credential.
  char accountName[40] = {0};   // The username: the identity when there is no id.
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
  // Checked: the server was asked about this hold and it stays until the book is opened. A
  // sleep session does not ask again until there is something new to ask about.
  bool checked(uint8_t index) const { return (checked_ >> index) & 1u; }
  void markChecked(const uint8_t md5[16]);
  // False if it was not marked: nothing to save.
  bool clearChecked(const uint8_t md5[16]);
  uint8_t size() const { return count_; }
  const uint8_t* at(uint8_t i) const { return items_[i]; }
  uint32_t evicted() const { return evicted_; }
  void clear() {
    count_ = 0;
    checked_ = 0;
  }

 private:
  friend class Store;
  uint8_t items_[kMax][16];
  uint8_t count_ = 0;
  uint32_t evicted_ = 0;
  uint32_t checked_ = 0;  // Bit i: items_[i]. kMax must fit.
};

struct SyncState {
  uint64_t cursor = 0;           // Everything up to here is applied and persisted.
  uint64_t committedCursor = 0;  // What the server has been told about it.
  uint16_t accountEpoch = 0;
  uint8_t failures = 0;
  bool needsRelink = false;      // 401/403: stop automatic sessions, keep the queue.
  uint32_t capsClientHash = 0;   // Non-zero: capabilities verified for this client_version.
  uint8_t linkFailStreak = 0;    // Automatic sessions in a row that never got a link.
  uint8_t linkSkipsLeft = 0;     // Automatic attempts to skip before trying the radio again.
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
  uint32_t revision;  // Newest revision seen from another device. 0: only ever touched here.
};

class Store {
 public:
  static constexpr uint16_t kMaxAnnotations = 512;

  // legacyKey: the chip's MAC, with which the previous firmware and format 1 of this one
  // obfuscated secrets. Only ever used to read those. box seals what is stored now.
  Store(IFs& fs, const uint8_t* legacyKey, size_t legacyKeyLen, ISecretBox& box);

  // Creates directories and loads everything. Imports the previous firmware's credential
  // once: only when no credential record of this firmware has ever existed, so never
  // again after a Disconnect. A record in the old format is resealed here and its slots
  // overwritten. False only on a hard IO error.
  bool begin();

  Credentials& creds() { return creds_; }
  const Credentials& creds() const { return creds_; }
  bool saveCredentials();

  SyncState& state() { return state_; }
  const SyncState& state() const { return state_; }
  bool saveState();

  // A pairing resolved to this account, by id when the server sent one and by username
  // otherwise. Anything but proof of the same account moves the epoch and empties the
  // queue, the cursor and the holds: that data may belong to someone else, and none of it
  // can go up under this login. The same account keeps everything.
  bool adoptAccount(const char* accountId, const char* accountName, Outbox& outbox);
  // Forgets the credential, with no copy left in either slot of the record. Keeps which
  // account it was (re-pairing it keeps the queue) and the sequence counter.
  bool forgetCredential();

  // A blank ledger with its id set when there is none: only IO is a failure.
  bool loadBook(const BookId& id, BookLedger& out);
  bool saveBook(const BookLedger& ledger);

  // Allocates exactly what is needed. Returns false on IO error or lack of memory.
  // *records must be released with delete[].
  bool loadAnnotations(const BookId& id, AnnotationRecord** records, uint16_t* count);
  bool saveAnnotations(const BookId& id, const AnnotationRecord* records, uint16_t count);
  // Someone else changed this annotation. A clipping this device never published is not its
  // business, so that costs no write. False only on IO.
  bool noteAnnotationRevision(const BookId& id, const uint8_t syncId[16], uint64_t revision);

  IFs& fs() { return fs_; }

 private:
  void bookPath(const char* subdir, const BookId& id, char* out) const;
  // Saves, creating the shard directory the first time this book needs it.
  bool saveShardRecord(AtomicFile& file, const char* path, const void* payload, size_t len);
  bool importLegacy();
  bool decodeLegacy(const char* path);
  void obfuscate(char* text, size_t len) const;
  bool writeCredentials(bool exclusive);
  bool readCredentialsV1(ByteReader& r);
  bool readCredentialsV2(ByteReader& r);

  IFs& fs_;
  ISecretBox& box_;
  uint8_t key_[8];
  uint8_t keyLen_;
  AtomicFile credFile_;
  AtomicFile stateFile_;
  AtomicFile bookFile_;
  AtomicFile annFile_;
  Credentials creds_;
  SyncState state_;
};

}  // namespace borges
