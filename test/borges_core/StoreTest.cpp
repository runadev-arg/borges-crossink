#include "TestSupport.h"

using namespace borges;
using namespace borges_test;

namespace {

// The previous firmware's obfuscation: XOR with the chip MAC, then base64.
std::string legacyObfuscate(const std::string& secret) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string xored = secret;
  for (size_t i = 0; i < xored.size(); ++i) xored[i] = static_cast<char>(xored[i] ^ testKey()[i % 6]);
  std::string out;
  uint32_t acc = 0;
  int bits = 0;
  for (unsigned char c : xored) {
    acc = (acc << 8) | c;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out += alphabet[(acc >> bits) & 0x3F];
    }
    acc &= (1u << bits) - 1u;
  }
  if (bits > 0) out += alphabet[(acc << (6 - bits)) & 0x3F];
  while (out.size() % 4) out += '=';
  return out;
}

constexpr const char* kLegacyToken = "toto_LEGACY12_abcdefghijklmnopqrstuvwxyzABCDEFGHIJK";

}  // namespace

TEST(Store, CredentialsRoundTripAndTokenIsNotInTheClear) {
  MemFs fs;
  {
    Store store(fs, testKey(), 6);
    ASSERT_TRUE(store.begin());
    EXPECT_STREQ(store.creds().baseUrl, kDefaultBaseUrl);
    copyText(store.creds().deviceId, sizeof(store.creds().deviceId), kSelfId);
    copyText(store.creds().token, sizeof(store.creds().token), "toto_SECRET");
    store.creds().expiresAtEpoch = 123;
    ASSERT_TRUE(store.saveCredentials());
  }
  for (const char* slot : {"/.crosspoint/borges/cred.a", "/.crosspoint/borges/cred.b"}) {
    EXPECT_EQ(fs.content(slot).find("toto_SECRET"), std::string::npos);
  }
  Store again(fs, testKey(), 6);
  ASSERT_TRUE(again.begin());
  EXPECT_TRUE(again.creds().paired());
  EXPECT_STREQ(again.creds().token, "toto_SECRET");
  EXPECT_EQ(again.creds().expiresAtEpoch, 123);
}

TEST(Store, TokenWithBytesEqualToTheKeyRoundTrips) {
  // XOR of a byte with itself is NUL: lengths must come from the record, not strlen.
  MemFs fs;
  std::string token;
  for (int i = 0; i < 6; ++i) token += static_cast<char>(testKey()[i] & 0x7F);
  const uint8_t key[6] = {'t', 'o', 't', 'o', '_', 'X'};
  {
    Store store(fs, key, 6);
    ASSERT_TRUE(store.begin());
    copyText(store.creds().deviceId, sizeof(store.creds().deviceId), kSelfId);
    copyText(store.creds().token, sizeof(store.creds().token), "toto_X_end");
    ASSERT_TRUE(store.saveCredentials());
  }
  Store again(fs, key, 6);
  ASSERT_TRUE(again.begin());
  EXPECT_STREQ(again.creds().token, "toto_X_end");
}

TEST(Store, AnUnreadableCardIsNotAnEmptyOne) {
  MemFs fs;
  {
    Store store(fs, testKey(), 6);
    ASSERT_TRUE(store.begin());
    copyText(store.creds().deviceId, sizeof(store.creds().deviceId), kSelfId);
    copyText(store.creds().token, sizeof(store.creds().token), "t");
    ASSERT_TRUE(store.saveCredentials());
    store.state().cursor = 9;
    ASSERT_TRUE(store.saveState());
    BookLedger ledger;
    ASSERT_TRUE(store.loadBook(Device::book(), ledger));
    ASSERT_TRUE(store.saveBook(ledger));
  }
  {
    fs.failReadsAfter("/.crosspoint/borges/cred.a", 0);
    Store store(fs, testKey(), 6);
    EXPECT_FALSE(store.begin());
  }
  {
    fs.failReadsAfter("/.crosspoint/borges/state.a", 0);
    Store store(fs, testKey(), 6);
    EXPECT_FALSE(store.begin());
  }
  fs.failReadsAfter("", 0);
  Store store(fs, testKey(), 6);
  ASSERT_TRUE(store.begin());
  EXPECT_EQ(store.state().cursor, 9u);
  fs.failReadsAfter(std::string("/.crosspoint/borges/books/") + kMd5A + ".a", 0);
  BookLedger ledger;
  EXPECT_FALSE(store.loadBook(Device::book(), ledger));
}

TEST(Store, StateRoundTrip) {
  MemFs fs;
  {
    Store store(fs, testKey(), 6);
    ASSERT_TRUE(store.begin());
    SyncState& s = store.state();
    s.cursor = 9007199254740993ULL;  // Above 2^53.
    s.committedCursor = 40;
    s.failures = 3;
    s.needsRelink = true;
    s.nextAttemptEpoch = kNow;
    copyText(s.diag.code, sizeof(s.diag.code), "transport");
    copyText(s.diag.requestId, sizeof(s.diag.requestId), "abc");
    s.diag.stage = SyncStage::Drain;
    s.holds.add(Device::book().md5);
    ASSERT_TRUE(store.saveState());
  }
  Store again(fs, testKey(), 6);
  ASSERT_TRUE(again.begin());
  const SyncState& s = again.state();
  EXPECT_EQ(s.cursor, 9007199254740993ULL);
  EXPECT_EQ(s.committedCursor, 40u);
  EXPECT_EQ(s.failures, 3);
  EXPECT_TRUE(s.needsRelink);
  EXPECT_EQ(s.nextAttemptEpoch, kNow);
  EXPECT_STREQ(s.diag.code, "transport");
  EXPECT_EQ(s.diag.stage, SyncStage::Drain);
  EXPECT_TRUE(s.holds.contains(Device::book().md5));
}

TEST(Store, HoldSetEvictsOldestWhenFullAndCounts) {
  HoldSet holds;
  uint8_t md5[16] = {0};
  for (int i = 0; i < HoldSet::kMax + 2; ++i) {
    md5[0] = static_cast<uint8_t>(i);
    EXPECT_TRUE(holds.add(md5));
  }
  EXPECT_EQ(holds.size(), HoldSet::kMax);
  EXPECT_EQ(holds.evicted(), 2u);
  md5[0] = 0;
  EXPECT_FALSE(holds.contains(md5));
  md5[0] = 2;
  EXPECT_TRUE(holds.contains(md5));
  EXPECT_FALSE(holds.add(md5));
  EXPECT_TRUE(holds.remove(md5));
  EXPECT_FALSE(holds.remove(md5));
}

TEST(Store, BookLedgerRoundTripWithLongestValues) {
  MemFs fs;
  Store store(fs, testKey(), 6);
  ASSERT_TRUE(store.begin());
  BookLedger l;
  ASSERT_TRUE(store.loadBook(Device::book(), l));
  l.inboundSeq = 99;
  const std::string longXp(kMaxXpointer - 1, 'x');
  l.head.valid = true;
  l.head.serverSeq = 77;
  copyText(l.head.position.xpointer, kMaxXpointer, longXp.c_str());
  l.head.position.hasPct = true;
  l.head.position.pctMicro = 500;
  copyText(l.head.deviceName, sizeof(l.head.deviceName), std::string(47, 'n').c_str());
  copyText(l.head.occurredAt, sizeof(l.head.occurredAt), std::string(31, 'o').c_str());
  l.head.precision = TimePrecision::Anchored;
  l.hasUndo = true;
  copyText(l.undo.xpointer, kMaxXpointer, longXp.c_str());
  l.hasFinalized = true;
  copyText(l.lastFinalized.xpointer, kMaxXpointer, longXp.c_str());
  copyText(l.suggestionId, sizeof(l.suggestionId), kOtherId);
  l.suggestionAction = SuggestionAction::Dismiss;
  for (int i = 0; i < 45; ++i) l.remember(static_cast<uint64_t>(i + 1));
  ASSERT_TRUE(store.saveBook(l));

  BookLedger r;
  ASSERT_TRUE(store.loadBook(Device::book(), r));
  EXPECT_EQ(r.inboundSeq, 99u);
  EXPECT_EQ(std::string(r.head.position.xpointer), longXp);
  EXPECT_EQ(r.head.precision, TimePrecision::Anchored);
  EXPECT_EQ(r.suggestionAction, SuggestionAction::Dismiss);
  EXPECT_STREQ(r.suggestionId, kOtherId);
  EXPECT_EQ(r.choiceCount, BookLedger::kMaxChoices);
  EXPECT_FALSE(r.hasChoice(1));
  EXPECT_FALSE(r.hasChoice(5));
  EXPECT_TRUE(r.hasChoice(45));
}

TEST(Store, RememberingAgainMovesToNewestAndKeepsFortyBooksOfChoices) {
  BookLedger l;
  l.remember(1);
  l.remember(2);
  l.remember(1);
  EXPECT_EQ(l.choiceCount, 2);
  EXPECT_TRUE(l.hasChoice(1));
  EXPECT_EQ(l.choices[1], 1u);
}

TEST(Store, AnnotationLedgerRoundTrip) {
  MemFs fs;
  Store store(fs, testKey(), 6);
  ASSERT_TRUE(store.begin());
  AnnotationRecord records[2]{};
  records[0].key = 5;
  records[0].syncId[0] = 0xAA;
  records[1].key = 6;
  records[1].bookmark = 1;
  ASSERT_TRUE(store.saveAnnotations(Device::book(), records, 2));
  AnnotationRecord* out = nullptr;
  uint16_t count = 0;
  ASSERT_TRUE(store.loadAnnotations(Device::book(), &out, &count));
  ASSERT_EQ(count, 2);
  EXPECT_EQ(out[0].key, 5u);
  EXPECT_EQ(out[0].syncId[0], 0xAA);
  EXPECT_EQ(out[1].bookmark, 1);
  delete[] out;
}

namespace {
class Collect : public IInboundVisitor {
 public:
  std::vector<uint64_t> seqs;
  std::vector<std::string> texts;
  void onAnnotation(const InboundAnnotation& a) override {
    seqs.push_back(a.serverSeq);
    texts.push_back(a.text);
  }
};
}  // namespace

TEST(Store, InboxListsInOrderSkipsDuplicatesAndTornBytes) {
  MemFs fs;
  Store store(fs, testKey(), 6);
  ASSERT_TRUE(store.begin());
  const BookId book = Device::book();
  InboundAnnotation a;
  copyText(a.text, sizeof(a.text), "uno");
  a.serverSeq = 10;
  ASSERT_TRUE(store.appendInbound(book, a));
  // Redelivery after a crash before the ledger save.
  ASSERT_TRUE(store.appendInbound(book, a));
  const std::string path = std::string("/.crosspoint/borges/inbox/") + kMd5A;
  fs.put(path, fs.content(path) + fs.content(path).substr(0, 15));  // Torn tail.
  a.serverSeq = 11;
  copyText(a.text, sizeof(a.text), "dos");
  ASSERT_TRUE(store.appendInbound(book, a));

  InboundAnnotation scratch;
  Collect c;
  ASSERT_TRUE(store.forEachInbound(book, scratch, c));
  EXPECT_EQ(c.seqs, (std::vector<uint64_t>{10, 11}));
  EXPECT_EQ(c.texts, (std::vector<std::string>{"uno", "dos"}));
}

TEST(Store, InboxRotatesAndStaysBounded) {
  MemFs fs;
  Store store(fs, testKey(), 6);
  ASSERT_TRUE(store.begin());
  const BookId book = Device::book();
  InboundAnnotation a;
  copyText(a.text, sizeof(a.text), std::string(1000, 't').c_str());
  for (uint64_t i = 1; i <= 200; ++i) {
    a.serverSeq = i;
    ASSERT_TRUE(store.appendInbound(book, a));
  }
  const std::string path = std::string("/.crosspoint/borges/inbox/") + kMd5A;
  EXPECT_TRUE(fs.has(path + ".old"));
  EXPECT_LE(fs.content(path).size(), Store::kInboxRotateBytes + 2048);
  InboundAnnotation scratch;
  Collect c;
  ASSERT_TRUE(store.forEachInbound(book, scratch, c));
  EXPECT_EQ(c.seqs.back(), 200u);
  EXPECT_GT(c.seqs.size(), 20u);
  EXPECT_LT(c.seqs.size(), 200u);
}

TEST(Store, AccountChangeEmptiesQueueAndCursorButKeepsSequence) {
  Device d;
  d.boot();
  copyText(d.store->creds().accountId, sizeof(d.store->creds().accountId), "account-a");
  d.store->state().cursor = 55;
  d.store->state().holds.add(Device::book().md5);
  ASSERT_EQ(d.appendRaw("mine"), Outbox::AppendResult::Ok);
  BookLedger l;
  ASSERT_TRUE(d.store->loadBook(Device::book(), l));
  l.hasUndo = true;
  ASSERT_TRUE(d.store->saveBook(l));

  // Same account: nothing changes.
  ASSERT_TRUE(d.store->adoptAccount("account-a", *d.outbox));
  EXPECT_EQ(d.outbox->depth(), 1u);
  EXPECT_EQ(d.store->state().cursor, 55u);

  // Another account: its data must not be uploaded under the new one.
  ASSERT_TRUE(d.store->adoptAccount("account-b", *d.outbox));
  EXPECT_EQ(d.outbox->depth(), 0u);
  EXPECT_EQ(d.store->state().cursor, 0u);
  EXPECT_EQ(d.store->state().holds.size(), 0);
  EXPECT_EQ(d.outbox->nextSequence(), 2u);
  ASSERT_TRUE(d.store->loadBook(Device::book(), l));
  EXPECT_FALSE(l.hasUndo);
}

TEST(LegacyImport, ReadsThePreviousFirmwaresCredentialOnce) {
  MemFs fs;
  JsonDocument doc;
  doc["cfgVersion"] = 1;
  doc["baseUrl"] = "https://borges.runadev.com/";
  doc["deviceId"] = kSelfId;
  doc["token_obf"] = legacyObfuscate(kLegacyToken);
  doc["pairingRequestId"] = "";
  doc["pairingNonce_obf"] = "";
  doc["serverIp"] = "104.21.0.166";
  doc["serverIpHost"] = "borges.runadev.com";
  std::string json;
  serializeJson(doc, json);
  fs.put(kLegacyCredentialPath, json);

  {
    Store store(fs, testKey(), 6);
    ASSERT_TRUE(store.begin());
    EXPECT_TRUE(store.creds().paired());
    EXPECT_STREQ(store.creds().token, kLegacyToken);
    EXPECT_STREQ(store.creds().deviceId, kSelfId);
    EXPECT_STREQ(store.creds().baseUrl, "https://borges.runadev.com");
    EXPECT_STREQ(store.creds().serverIp, "104.21.0.166");
    EXPECT_STREQ(store.creds().serverIpHost, "borges.runadev.com");
  }
  // The legacy file stays, so a rollback still works, and the import does not rerun.
  EXPECT_TRUE(fs.has(kLegacyCredentialPath));
  fs.put(kLegacyCredentialPath, "{\"deviceId\":\"x\",\"token\":\"other\"}");
  Store again(fs, testKey(), 6);
  ASSERT_TRUE(again.begin());
  EXPECT_STREQ(again.creds().token, kLegacyToken);
}

TEST(LegacyImport, TheOlderFileNameAndAPlainTokenWork) {
  MemFs fs;
  fs.put(kLegacyCredentialPathOld, std::string("{\"deviceId\":\"") + kSelfId + "\",\"token\":\"plain_token\"}");
  Store store(fs, testKey(), 6);
  ASSERT_TRUE(store.begin());
  EXPECT_STREQ(store.creds().token, "plain_token");
  EXPECT_STREQ(store.creds().baseUrl, kDefaultBaseUrl);
}

TEST(LegacyImport, AFileFromAnotherChipIsIgnoredNotHalfImported) {
  MemFs fs;
  const uint8_t otherKey[6] = {1, 2, 3, 4, 5, 6};
  std::string xored = kLegacyToken;
  (void)otherKey;
  JsonDocument doc;
  doc["deviceId"] = kSelfId;
  // Decodes to non-printable bytes under this chip's key.
  doc["token_obf"] = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGw==";
  std::string json;
  serializeJson(doc, json);
  fs.put(kLegacyCredentialPath, json);
  Store store(fs, testKey(), 6);
  ASSERT_TRUE(store.begin());
  EXPECT_FALSE(store.creds().paired());
  EXPECT_EQ(store.creds().token[0], '\0');
}

TEST(LegacyImport, HostileFilesDoNothing) {
  const std::vector<std::string> files = {
      "",
      "not json",
      "{\"deviceId\":",
      "[1,2,3]",
      "{\"deviceId\":\"\",\"token\":\"t\"}",
      "{\"deviceId\":\"d\"}",
      "{\"deviceId\":12,\"token\":34}",
      "{\"deviceId\":\"d\",\"token\":\"has space\"}",
      std::string(10000, ' '),
  };
  for (const std::string& file : files) {
    MemFs fs;
    fs.put(kLegacyCredentialPath, file);
    Store store(fs, testKey(), 6);
    ASSERT_TRUE(store.begin());
    EXPECT_FALSE(store.creds().paired()) << file.substr(0, 40);
  }
}

TEST(LegacyImport, ABadBaseUrlKeepsTheDefault) {
  MemFs fs;
  fs.put(kLegacyCredentialPath, std::string("{\"baseUrl\":\"http://evil.example\",\"deviceId\":\"") + kSelfId + "\",\"token\":\"t\"}");
  Store store(fs, testKey(), 6);
  ASSERT_TRUE(store.begin());
  EXPECT_TRUE(store.creds().paired());
  EXPECT_STREQ(store.creds().baseUrl, kDefaultBaseUrl);
}
