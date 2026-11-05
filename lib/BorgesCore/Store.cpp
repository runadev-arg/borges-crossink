#include "Store.h"

#include <cstring>
#include <new>

#include "Codec.h"
#include "JsonStream.h"

namespace borges {

namespace {

constexpr uint8_t kCredVersion = 1;
constexpr uint8_t kStateVersion = 1;
constexpr uint8_t kBookVersion = 3;
constexpr uint8_t kAnnVersion = 1;
constexpr size_t kLegacyMaxBytes = 8 * 1024;
constexpr size_t kAnnHeaderBytes = 5;  // version u8, epoch u16, count u16
constexpr size_t kAnnRecordBytes = 25;

void writePosition(ByteWriter& w, const Position& p) {
  w.u8(p.hasPct ? 1 : 0);
  w.u32(p.pctMicro);
  w.u32(static_cast<uint32_t>(p.page));
  w.u32(static_cast<uint32_t>(p.totalPages));
  w.str(p.xpointer);
}

void readPosition(ByteReader& r, Position& p) {
  p.hasPct = r.u8() != 0;
  p.pctMicro = r.u32();
  p.page = static_cast<int32_t>(r.u32());
  p.totalPages = static_cast<int32_t>(r.u32());
  r.str(p.xpointer, sizeof(p.xpointer));
}

void writeDiag(ByteWriter& w, const Diagnostics& d) {
  w.u8(static_cast<uint8_t>(d.stage));
  w.u16(static_cast<uint16_t>(d.httpStatus));
  w.u8(static_cast<uint8_t>(d.transportStage));
  w.u32(static_cast<uint32_t>(d.transportCode));
  w.u64(static_cast<uint64_t>(d.atEpoch));
  w.str(d.code);
  w.str(d.detail);
  w.str(d.requestId);
}

void readDiag(ByteReader& r, Diagnostics& d) {
  d.stage = static_cast<SyncStage>(r.u8());
  d.httpStatus = static_cast<int16_t>(r.u16());
  d.transportStage = static_cast<TransportStage>(r.u8());
  d.transportCode = static_cast<int32_t>(r.u32());
  d.atEpoch = static_cast<int64_t>(r.u64());
  r.str(d.code, sizeof(d.code));
  r.str(d.detail, sizeof(d.detail));
  r.str(d.requestId, sizeof(d.requestId));
}

}  // namespace

bool HoldSet::isHeld(uint32_t bookTag) const {
  for (uint8_t i = 0; i < count_; ++i) {
    if (bookTagOf(items_[i]) == bookTag) return true;
  }
  return false;
}

bool HoldSet::contains(const uint8_t md5[16]) const {
  for (uint8_t i = 0; i < count_; ++i) {
    if (memcmp(items_[i], md5, 16) == 0) return true;
  }
  return false;
}

bool HoldSet::add(const uint8_t md5[16]) {
  if (contains(md5)) return false;
  if (count_ == kMax) {
    memmove(items_[0], items_[1], 16 * (kMax - 1));
    --count_;
    ++evicted_;
  }
  memcpy(items_[count_++], md5, 16);
  return true;
}

bool HoldSet::remove(const uint8_t md5[16]) {
  for (uint8_t i = 0; i < count_; ++i) {
    if (memcmp(items_[i], md5, 16) == 0) {
      memmove(items_[i], items_[i + 1], 16 * (count_ - i - 1u));
      --count_;
      return true;
    }
  }
  return false;
}

void BookLedger::remember(uint64_t fingerprint) {
  for (uint8_t i = 0; i < choiceCount; ++i) {
    if (choices[i] == fingerprint) {
      memmove(&choices[i], &choices[i + 1], sizeof(choices[0]) * (choiceCount - i - 1u));
      --choiceCount;
      break;
    }
  }
  if (choiceCount == kMaxChoices) {
    memmove(&choices[0], &choices[1], sizeof(choices[0]) * (kMaxChoices - 1));
    --choiceCount;
  }
  choices[choiceCount++] = fingerprint;
}

bool BookLedger::hasChoice(uint64_t fingerprint) const {
  for (uint8_t i = 0; i < choiceCount; ++i) {
    if (choices[i] == fingerprint) return true;
  }
  return false;
}

Store::Store(IFs& fs, const uint8_t* key, size_t keyLen)
    : fs_(fs),
      keyLen_(0),
      credFile_(fs, "/.crosspoint/borges/cred"),
      stateFile_(fs, "/.crosspoint/borges/state"),
      bookFile_(fs, ""),
      annFile_(fs, "") {
  keyLen_ = static_cast<uint8_t>(keyLen > sizeof(key_) ? sizeof(key_) : keyLen);
  if (key && keyLen_) memcpy(key_, key, keyLen_);
}

void Store::obfuscate(char* text, size_t len) const {
  if (keyLen_ == 0) return;
  for (size_t i = 0; i < len; ++i) text[i] = static_cast<char>(text[i] ^ key_[i % keyLen_]);
}

bool Store::begin() {
  fs_.mkdir("/.crosspoint");
  if (!fs_.mkdir(kBorgesDir)) return false;
  fs_.mkdir(kOutboxDir);
  fs_.mkdir("/.crosspoint/borges/spool");
  fs_.mkdir("/.crosspoint/borges/books");
  fs_.mkdir("/.crosspoint/borges/ann");
  fs_.mkdir("/.crosspoint/borges/catalog");

  creds_ = Credentials{};
  state_ = SyncState{};

  uint8_t buf[1024];
  size_t len = 0;
  bool haveCreds = false;
  if (credFile_.load(buf, sizeof(buf), len)) {
    ByteReader r(buf, len);
    if (r.u8() == kCredVersion) {
      r.str(creds_.baseUrl, sizeof(creds_.baseUrl));
      r.str(creds_.deviceId, sizeof(creds_.deviceId));
      const size_t tokenLen = r.str(creds_.token, sizeof(creds_.token));
      r.str(creds_.accountId, sizeof(creds_.accountId));
      creds_.expiresAtEpoch = static_cast<int64_t>(r.u64());
      creds_.renewAfterEpoch = static_cast<int64_t>(r.u64());
      r.str(creds_.serverIp, sizeof(creds_.serverIp));
      r.str(creds_.serverIpHost, sizeof(creds_.serverIpHost));
      r.str(creds_.pairing.requestId, sizeof(creds_.pairing.requestId));
      const size_t nonceLen = r.str(creds_.pairing.nonce, sizeof(creds_.pairing.nonce));
      r.str(creds_.pairing.userCode, sizeof(creds_.pairing.userCode));
      r.str(creds_.pairing.verificationUrl, sizeof(creds_.pairing.verificationUrl));
      creds_.pairing.intervalSec = r.u32();
      creds_.pairing.expiresAtEpoch = static_cast<int64_t>(r.u64());
      if (r.ok()) {
        // Lengths come from the record: an obfuscated byte may legitimately be NUL.
        obfuscate(creds_.token, tokenLen);
        obfuscate(creds_.pairing.nonce, nonceLen);
        haveCreds = true;
      } else {
        creds_ = Credentials{};
      }
    }
  }
  // An unreadable card is not an empty one: starting blank here would look unpaired,
  // and later writes could lose to the slots that could not be read.
  if (credFile_.ioError()) return false;
  if (!haveCreds) copyText(creds_.baseUrl, sizeof(creds_.baseUrl), kDefaultBaseUrl);

  if (stateFile_.load(buf, sizeof(buf), len)) {
    ByteReader r(buf, len);
    SyncState s;
    if (r.u8() == kStateVersion) {
      s.cursor = r.u64();
      s.committedCursor = r.u64();
      s.accountEpoch = r.u16();
      s.failures = r.u8();
      s.needsRelink = r.u8() != 0;
      s.capsClientHash = r.u32();
      s.nextAttemptEpoch = static_cast<int64_t>(r.u64());
      s.anchorEpoch = static_cast<int64_t>(r.u64());
      s.lastSuccessEpoch = static_cast<int64_t>(r.u64());
      s.skippedEvents = r.u32();
      s.lastSkippedSeq = r.u64();
      s.rejectedEvents = r.u32();
      r.str(s.lastRejection, sizeof(s.lastRejection));
      readDiag(r, s.diag);
      s.holds.evicted_ = r.u32();
      const uint8_t holds = r.u8();
      if (holds <= HoldSet::kMax) {
        for (uint8_t i = 0; i < holds; ++i) r.bytes(s.holds.items_[i], 16);
        s.holds.count_ = holds;
      }
      if (r.ok()) state_ = s;
    }
  }
  if (stateFile_.ioError()) return false;

  if (!creds_.paired() && !creds_.pairing.active()) importLegacy();
  return true;
}

bool Store::saveCredentials() {
  uint8_t buf[1024];
  ByteWriter w(buf, sizeof(buf));
  w.u8(kCredVersion);
  w.str(creds_.baseUrl);
  w.str(creds_.deviceId);
  char token[sizeof(creds_.token)];
  const size_t tokenLen = strlen(creds_.token);
  memcpy(token, creds_.token, tokenLen);
  token[tokenLen] = '\0';
  obfuscate(token, tokenLen);
  w.u16(static_cast<uint16_t>(tokenLen));
  w.bytes(token, tokenLen);
  memset(token, 0, sizeof(token));
  w.str(creds_.accountId);
  w.u64(static_cast<uint64_t>(creds_.expiresAtEpoch));
  w.u64(static_cast<uint64_t>(creds_.renewAfterEpoch));
  w.str(creds_.serverIp);
  w.str(creds_.serverIpHost);
  w.str(creds_.pairing.requestId);
  char nonce[sizeof(creds_.pairing.nonce)];
  const size_t nonceLen = strlen(creds_.pairing.nonce);
  memcpy(nonce, creds_.pairing.nonce, nonceLen);
  obfuscate(nonce, nonceLen);
  w.u16(static_cast<uint16_t>(nonceLen));
  w.bytes(nonce, nonceLen);
  w.str(creds_.pairing.userCode);
  w.str(creds_.pairing.verificationUrl);
  w.u32(creds_.pairing.intervalSec);
  w.u64(static_cast<uint64_t>(creds_.pairing.expiresAtEpoch));
  if (!w.ok()) return false;
  return credFile_.save(buf, w.size());
}

bool Store::saveState() {
  uint8_t buf[1024];
  ByteWriter w(buf, sizeof(buf));
  const SyncState& s = state_;
  w.u8(kStateVersion);
  w.u64(s.cursor);
  w.u64(s.committedCursor);
  w.u16(s.accountEpoch);
  w.u8(s.failures);
  w.u8(s.needsRelink ? 1 : 0);
  w.u32(s.capsClientHash);
  w.u64(static_cast<uint64_t>(s.nextAttemptEpoch));
  w.u64(static_cast<uint64_t>(s.anchorEpoch));
  w.u64(static_cast<uint64_t>(s.lastSuccessEpoch));
  w.u32(s.skippedEvents);
  w.u64(s.lastSkippedSeq);
  w.u32(s.rejectedEvents);
  w.str(s.lastRejection);
  writeDiag(w, s.diag);
  w.u32(s.holds.evicted());
  w.u8(s.holds.size());
  for (uint8_t i = 0; i < s.holds.size(); ++i) w.bytes(s.holds.at(i), 16);
  if (!w.ok()) return false;
  return stateFile_.save(buf, w.size());
}

bool Store::adoptAccount(const char* accountId, Outbox& outbox) {
  if (!accountId || !*accountId) return true;
  if (creds_.accountId[0] && strcmp(creds_.accountId, accountId) != 0) {
    if (outbox.isOpen() && !outbox.reset()) return false;
    state_.cursor = 0;
    state_.committedCursor = 0;
    state_.holds.clear();
    state_.accountEpoch = static_cast<uint16_t>(state_.accountEpoch + 1);
    state_.capsClientHash = 0;
    if (!saveState()) return false;
  }
  copyText(creds_.accountId, sizeof(creds_.accountId), accountId);
  return true;
}

bool Store::forgetCredential() {
  creds_.deviceId[0] = '\0';
  creds_.token[0] = '\0';
  creds_.expiresAtEpoch = 0;
  creds_.renewAfterEpoch = 0;
  creds_.pairing = PendingPairing{};
  return saveCredentials();
}

void Store::bookPath(const char* subdir, const BookId& id, char* out) const {
  const size_t a = strlen(kBorgesDir);
  memcpy(out, kBorgesDir, a);
  out[a] = '/';
  const size_t b = strlen(subdir);
  memcpy(out + a + 1, subdir, b);
  char* name = out + a + 1 + b;
  name[0] = '/';
  name[1] = id.hex[0];
  name[2] = id.hex[1];
  name[3] = '/';
  memcpy(name + 4, id.hex + 2, 31);
}

bool Store::saveShardRecord(AtomicFile& file, const char* path, const void* payload, size_t len) {
  if (file.save(payload, len)) return true;
  // The common cause of a failed first save, and a lookup not worth paying on every save.
  char dir[AtomicFile::kMaxPath];
  const char* slash = strrchr(path, '/');
  const size_t n = static_cast<size_t>(slash - path);
  memcpy(dir, path, n);
  dir[n] = '\0';
  return fs_.mkdir(dir) && file.save(payload, len);
}

bool Store::loadBook(const BookId& id, BookLedger& out) {
  out = BookLedger{};
  out.id = id;
  char path[AtomicFile::kMaxPath];
  bookPath("books", id, path);
  AtomicFile& file = bookFile_;
  file.setPath(path);
  uint8_t* buf = new (std::nothrow) uint8_t[2400];
  if (!buf) return false;
  size_t len = 0;
  if (file.load(buf, 2400, len)) {
    ByteReader r(buf, len);
    const uint8_t version = r.u8();
    const uint16_t epoch = r.u16();
    if (version == kBookVersion && epoch == state_.accountEpoch) {
      BookLedger* l = &out;
      l->head.valid = r.u8() != 0;
      l->head.serverSeq = r.u64();
      readPosition(r, l->head.position);
      l->head.precision = static_cast<TimePrecision>(r.u8());
      r.str(l->head.occurredAt, sizeof(l->head.occurredAt));
      r.str(l->head.deviceName, sizeof(l->head.deviceName));
      l->hasUndo = r.u8() != 0;
      readPosition(r, l->undo);
      l->hasFinalized = r.u8() != 0;
      readPosition(r, l->lastFinalized);
      l->lastTurnEpoch = static_cast<int64_t>(r.u64());
      r.str(l->suggestionId, sizeof(l->suggestionId));
      l->suggestionAction = static_cast<SuggestionAction>(r.u8());
      const uint8_t choices = r.u8();
      if (choices <= BookLedger::kMaxChoices) {
        l->choiceCount = choices;
        for (uint8_t i = 0; i < choices; ++i) l->choices[i] = r.u64();
      } else {
        delete[] buf;
        out = BookLedger{};
        out.id = id;
        return true;
      }
      if (!r.ok()) {
        out = BookLedger{};
        out.id = id;
      }
    }
  }
  delete[] buf;
  return !file.ioError();
}

bool Store::saveBook(const BookLedger& l) {
  char path[AtomicFile::kMaxPath];
  bookPath("books", l.id, path);
  bookFile_.usePath(path);
  uint8_t* buf = new (std::nothrow) uint8_t[2400];
  if (!buf) return false;
  ByteWriter w(buf, 2400);
  w.u8(kBookVersion);
  w.u16(state_.accountEpoch);
  w.u8(l.head.valid ? 1 : 0);
  w.u64(l.head.serverSeq);
  writePosition(w, l.head.position);
  w.u8(static_cast<uint8_t>(l.head.precision));
  w.str(l.head.occurredAt);
  w.str(l.head.deviceName);
  w.u8(l.hasUndo ? 1 : 0);
  writePosition(w, l.undo);
  w.u8(l.hasFinalized ? 1 : 0);
  writePosition(w, l.lastFinalized);
  w.u64(static_cast<uint64_t>(l.lastTurnEpoch));
  w.str(l.suggestionId);
  w.u8(static_cast<uint8_t>(l.suggestionAction));
  w.u8(l.choiceCount);
  for (uint8_t i = 0; i < l.choiceCount; ++i) w.u64(l.choices[i]);
  const bool ok = w.ok() && saveShardRecord(bookFile_, path, buf, w.size());
  delete[] buf;
  return ok;
}

bool Store::loadAnnotations(const BookId& id, AnnotationRecord** records, uint16_t* count) {
  *records = nullptr;
  *count = 0;
  char path[AtomicFile::kMaxPath];
  bookPath("ann", id, path);
  annFile_.setPath(path);
  // Sized from what is stored: a book with no clippings allocates nothing here, and there
  // is no maximum for this to fall short of.
  const size_t stored = annFile_.payloadSize();
  if (annFile_.ioError()) return false;  // Not "no annotations": that would republish every clipping.
  uint8_t* buf = nullptr;
  size_t len = 0;
  uint16_t n = 0;
  uint16_t epoch = 0;
  if (stored >= kAnnHeaderBytes && stored <= kAnnHeaderBytes + kMaxAnnotations * kAnnRecordBytes) {
    buf = new (std::nothrow) uint8_t[stored];
    if (!buf) return false;
    const bool loaded = annFile_.load(buf, stored, len);
    if (annFile_.ioError()) {
      delete[] buf;
      return false;
    }
    if (loaded) {
      ByteReader r(buf, len);
      if (r.u8() == kAnnVersion) {
        epoch = r.u16();
        n = r.u16();
        if (epoch != state_.accountEpoch || n > kMaxAnnotations || r.remaining() != n * kAnnRecordBytes) n = 0;
      }
    }
  }
  // Room for one more per clipping found later is handled by the caller, which
  // always allocates its own working copy.
  AnnotationRecord* out = new (std::nothrow) AnnotationRecord[n ? n : 1];
  if (!out) {
    delete[] buf;
    return false;
  }
  if (n) {
    ByteReader r(buf + kAnnHeaderBytes, len - kAnnHeaderBytes);
    for (uint16_t i = 0; i < n; ++i) {
      out[i].key = r.u64();
      r.bytes(out[i].syncId, 16);
      out[i].bookmark = r.u8();
    }
  }
  delete[] buf;
  *records = out;
  *count = n;
  return true;
}

bool Store::saveAnnotations(const BookId& id, const AnnotationRecord* records, uint16_t count) {
  if (count > kMaxAnnotations) return false;
  char path[AtomicFile::kMaxPath];
  bookPath("ann", id, path);
  annFile_.usePath(path);
  const size_t cap = kAnnHeaderBytes + static_cast<size_t>(count) * kAnnRecordBytes;
  uint8_t* buf = new (std::nothrow) uint8_t[cap];
  if (!buf) return false;
  ByteWriter w(buf, cap);
  w.u8(kAnnVersion);
  w.u16(state_.accountEpoch);
  w.u16(count);
  for (uint16_t i = 0; i < count; ++i) {
    w.u64(records[i].key);
    w.bytes(records[i].syncId, 16);
    w.u8(records[i].bookmark);
  }
  const bool ok = w.ok() && saveShardRecord(annFile_, path, buf, w.size());
  delete[] buf;
  return ok;
}

// The previous firmware kept its credential as JSON. Field names and the obfuscation
// (base64 of the secret XORed with the chip MAC) are that firmware's, read-only here.
// A file that does not decode to a printable token is ignored, not half-imported:
// bytes from the wrong key must not go out in an Authorization header.
bool Store::importLegacy() {
  const char* path = kLegacyCredentialPath;
  if (!fs_.exists(path)) {
    path = kLegacyCredentialPathOld;
    if (!fs_.exists(path)) return false;
  }
  File f(fs_, path, IFs::Mode::Read);
  if (!f || f.size() > static_cast<int64_t>(kLegacyMaxBytes)) return false;
  FileByteSource source(f);
  JsonScanner scan(source);
  if (!scan.enterObject()) return false;

  char key[32];
  char baseUrl[sizeof(creds_.baseUrl)] = {0};
  char deviceId[sizeof(creds_.deviceId)] = {0};
  char tokenObf[256] = {0};
  char tokenPlain[sizeof(creds_.token)] = {0};
  char serverIp[sizeof(creds_.serverIp)] = {0};
  char serverIpHost[sizeof(creds_.serverIpHost)] = {0};
  while (scan.nextMember(key, sizeof(key))) {
    if (strcmp(key, "baseUrl") == 0) {
      scan.readString(baseUrl, sizeof(baseUrl));
    } else if (strcmp(key, "deviceId") == 0) {
      scan.readString(deviceId, sizeof(deviceId));
    } else if (strcmp(key, "token_obf") == 0) {
      scan.readString(tokenObf, sizeof(tokenObf));
    } else if (strcmp(key, "token") == 0) {
      scan.readString(tokenPlain, sizeof(tokenPlain));
    } else if (strcmp(key, "serverIp") == 0) {
      scan.readString(serverIp, sizeof(serverIp));
    } else if (strcmp(key, "serverIpHost") == 0) {
      scan.readString(serverIpHost, sizeof(serverIpHost));
    } else {
      scan.skipValue();
    }
  }
  if (scan.failed() || !deviceId[0]) return false;

  char token[sizeof(creds_.token)] = {0};
  if (tokenObf[0]) {
    const int32_t n = base64Decode(tokenObf, reinterpret_cast<uint8_t*>(token), sizeof(token) - 1);
    if (n > 0) {
      obfuscate(token, static_cast<size_t>(n));
      token[n] = '\0';
    } else {
      token[0] = '\0';
    }
  }
  if (!token[0]) copyText(token, sizeof(token), tokenPlain);
  if (!token[0]) return false;
  for (const char* p = token; *p; ++p) {
    if (*p < 0x21 || *p > 0x7E) return false;
  }

  size_t urlLen = strlen(baseUrl);
  while (urlLen > 0 && baseUrl[urlLen - 1] == '/') baseUrl[--urlLen] = '\0';
  if (strncmp(baseUrl, "https://", 8) == 0 && urlLen > 8) {
    copyText(creds_.baseUrl, sizeof(creds_.baseUrl), baseUrl);
  }
  copyText(creds_.deviceId, sizeof(creds_.deviceId), deviceId);
  copyText(creds_.token, sizeof(creds_.token), token);
  memset(token, 0, sizeof(token));
  copyText(creds_.serverIp, sizeof(creds_.serverIp), serverIp);
  copyText(creds_.serverIpHost, sizeof(creds_.serverIpHost), serverIpHost);
  return saveCredentials();
}

}  // namespace borges
