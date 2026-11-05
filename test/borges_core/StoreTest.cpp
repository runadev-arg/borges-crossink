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
    Store store(fs, testKey(), 6, testBox());
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
  Store again(fs, testKey(), 6, testBox());
  ASSERT_TRUE(again.begin());
  EXPECT_TRUE(again.creds().paired());
  EXPECT_STREQ(again.creds().token, "toto_SECRET");
  EXPECT_EQ(again.creds().expiresAtEpoch, 123);
}

namespace {

// A format 1 record: what a card written by the first Borges build holds.
std::string credV1(const std::string& token, const std::string& account, const std::string& url = kDefaultBaseUrl) {
  std::string xored = token;
  for (size_t i = 0; i < xored.size(); ++i) xored[i] = static_cast<char>(xored[i] ^ testKey()[i % 6]);
  std::string raw;
  const auto u16 = [&](size_t v) {
    raw += static_cast<char>(v & 0xFF);
    raw += static_cast<char>(v >> 8);
  };
  const auto str = [&](const std::string& v) {
    u16(v.size());
    raw += v;
  };
  raw += static_cast<char>(1);
  str(url);
  str(kSelfId);
  str(xored);
  str(account);
  raw.append(16, '\0');  // expires, renew
  str("");                // serverIp
  str("");                // serverIpHost
  str("");                // requestId
  str("");                // nonce
  str("");                // userCode
  str("");                // verificationUrl
  raw.append(12, '\0');  // interval, pairing expiry
  return raw;
}

void saveV1(MemFs& fs, const std::string& token, const std::string& account) {
  AtomicFile file(fs, "/.crosspoint/borges/cred");
  const std::string raw = credV1(token, account);
  ASSERT_TRUE(file.save(raw.data(), raw.size()));
}

std::string credSlots(MemFs& fs) {
  return fs.content("/.crosspoint/borges/cred.a") + fs.content("/.crosspoint/borges/cred.b");
}

}  // namespace

TEST(Store, ATokenInTheOldFormatIsResealedAndNoCopyStaysOnTheCard) {
  MemFs fs;
  fs.mkdir("/.crosspoint");
  const std::string token = "toto_OLD_FORMAT_TOKEN";
  saveV1(fs, "older", "account-1");  // Slot a.
  saveV1(fs, token, "account-1");    // Slot b, the newest.
  std::string xored = token;
  for (size_t i = 0; i < xored.size(); ++i) xored[i] = static_cast<char>(xored[i] ^ testKey()[i % 6]);
  ASSERT_NE(credSlots(fs).find(xored), std::string::npos);
  {
    Store store(fs, testKey(), 6, testBox());
    ASSERT_TRUE(store.begin());
    EXPECT_STREQ(store.creds().token, token.c_str());
    EXPECT_STREQ(store.creds().accountId, "account-1");
  }
  EXPECT_EQ(credSlots(fs).find(xored), std::string::npos) << "an XORed token is one MAC away from the clear";
  EXPECT_EQ(credSlots(fs).find(token), std::string::npos);
  Store again(fs, testKey(), 6, testBox());
  ASSERT_TRUE(again.begin());
  EXPECT_STREQ(again.creds().token, token.c_str());
}

TEST(Store, ACardReadOnAnotherChipYieldsNoToken) {
  MemFs fs;
  {
    Store store(fs, testKey(), 6, testBox());
    ASSERT_TRUE(store.begin());
    copyText(store.creds().deviceId, sizeof(store.creds().deviceId), kSelfId);
    copyText(store.creds().token, sizeof(store.creds().token), "toto_SECRET");
    copyText(store.creds().accountId, sizeof(store.creds().accountId), "account-1");
    ASSERT_TRUE(store.saveCredentials());
  }
  // The MAC is printed on the box: knowing it must not be enough.
  FakeRandom random;
  const uint8_t otherKey[32] = {9, 9, 9};
  SoftSecretBox otherChip(otherKey, random);
  Store store(fs, testKey(), 6, otherChip);
  ASSERT_TRUE(store.begin());
  EXPECT_FALSE(store.creds().paired());
  EXPECT_EQ(store.creds().token[0], '\0');
  EXPECT_STREQ(store.creds().accountId, "account-1") << "it still knows whose queue it holds";
}

TEST(Store, AFlippedByteInTheSealedPartIsNotAToken) {
  FakeRandom random;
  SoftSecretBox box(testNvsKey(), random);
  const char plain[] = "secret";
  uint8_t sealed[sizeof(plain) + ISecretBox::kOverhead];
  ASSERT_TRUE(box.seal(reinterpret_cast<const uint8_t*>(plain), sizeof(plain), sealed));
  char out[sizeof(plain)];
  ASSERT_TRUE(box.open(sealed, sizeof(sealed), reinterpret_cast<uint8_t*>(out)));
  EXPECT_STREQ(out, plain);
  sealed[14] ^= 1;
  EXPECT_FALSE(box.open(sealed, sizeof(sealed), reinterpret_cast<uint8_t*>(out)));
}

TEST(Store, WithNoWayToSealTheTokenItIsNotWrittenAtAll) {
  class BrokenBox : public ISecretBox {
   public:
    bool seal(const uint8_t*, size_t, uint8_t*) override { return false; }
    bool open(const uint8_t*, size_t, uint8_t*) override { return false; }
  } broken;
  MemFs fs;
  Store store(fs, testKey(), 6, broken);
  ASSERT_TRUE(store.begin());
  copyText(store.creds().deviceId, sizeof(store.creds().deviceId), kSelfId);
  copyText(store.creds().token, sizeof(store.creds().token), "toto_SECRET");
  EXPECT_FALSE(store.saveCredentials());
  EXPECT_EQ(credSlots(fs).find("toto_SECRET"), std::string::npos);
}

TEST(Store, ForgetLeavesNoCopyOfTheTokenInEitherSlot) {
  MemFs fs;
  Store store(fs, testKey(), 6, testBox());
  ASSERT_TRUE(store.begin());
  copyText(store.creds().deviceId, sizeof(store.creds().deviceId), kSelfId);
  copyText(store.creds().token, sizeof(store.creds().token), "toto_SECRET");
  ASSERT_TRUE(store.saveCredentials());
  ASSERT_TRUE(store.saveCredentials());  // Both slots hold a sealed copy now.
  const std::string sealedBefore = fs.content("/.crosspoint/borges/cred.a");
  ASSERT_GT(sealedBefore.size(), 100u);

  ASSERT_TRUE(store.forgetCredential());
  int slots = 0;
  for (const char* slot : {"/.crosspoint/borges/cred.a", "/.crosspoint/borges/cred.b"}) {
    if (!fs.has(slot)) continue;
    ++slots;
    EXPECT_LT(fs.content(slot).size(), sealedBefore.size()) << "no sealed part left in " << slot;
  }
  EXPECT_EQ(slots, 1);
  Store again(fs, testKey(), 6, testBox());
  ASSERT_TRUE(again.begin());
  EXPECT_FALSE(again.creds().paired());
}

TEST(Store, ScrubOverwritesInPlaceBeforeRemoving) {
  MemFs fs;
  fs.put("/spool", "token token token");
  uint64_t writes = 0;
  scrubFile(fs, "/spool");
  writes = fs.writeOpsTo("/spool");
  EXPECT_GE(writes, 1u) << "removing alone leaves the bytes on the card";
  EXPECT_FALSE(fs.has("/spool"));
  scrubFile(fs, "/not-there");
}

TEST(Store, AnUnreadableCardIsNotAnEmptyOne) {
  MemFs fs;
  {
    Store store(fs, testKey(), 6, testBox());
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
    Store store(fs, testKey(), 6, testBox());
    EXPECT_FALSE(store.begin());
  }
  {
    fs.failReadsAfter("/.crosspoint/borges/state.a", 0);
    Store store(fs, testKey(), 6, testBox());
    EXPECT_FALSE(store.begin());
  }
  fs.failReadsAfter("", 0);
  Store store(fs, testKey(), 6, testBox());
  ASSERT_TRUE(store.begin());
  EXPECT_EQ(store.state().cursor, 9u);
  fs.failReadsAfter(std::string("/.crosspoint/borges/books/ab/") + (kMd5A + 2) + ".a", 0);
  BookLedger ledger;
  EXPECT_FALSE(store.loadBook(Device::book(), ledger));
}

TEST(Store, StateRoundTrip) {
  MemFs fs;
  {
    Store store(fs, testKey(), 6, testBox());
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
  Store again(fs, testKey(), 6, testBox());
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
  Store store(fs, testKey(), 6, testBox());
  ASSERT_TRUE(store.begin());
  BookLedger l;
  ASSERT_TRUE(store.loadBook(Device::book(), l));
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

TEST(Store, PerBookRecordsLiveInShardDirectoriesCreatedOnDemand) {
  MemFs fs;
  fs.requireDirs();
  Store store(fs, testKey(), 6, testBox());
  ASSERT_TRUE(store.begin());
  BookLedger ledger;
  ASSERT_TRUE(store.loadBook(Device::book(), ledger));
  ledger.hasUndo = true;
  ASSERT_TRUE(store.saveBook(ledger));
  AnnotationRecord record{};
  record.key = 7;
  ASSERT_TRUE(store.saveAnnotations(Device::book(), &record, 1));
  const std::string rest = kMd5A + 2;
  EXPECT_TRUE(fs.has("/.crosspoint/borges/books/ab/" + rest + ".a"));
  EXPECT_TRUE(fs.has("/.crosspoint/borges/ann/ab/" + rest + ".a"));
  ASSERT_TRUE(store.loadBook(Device::book(), ledger));
  EXPECT_TRUE(ledger.hasUndo);
}

TEST(Store, ALoadThenSaveOfAPerBookRecordDoesNotLookTwice) {
  MemFs fs;
  Store store(fs, testKey(), 6, testBox());
  ASSERT_TRUE(store.begin());
  BookLedger ledger;
  ASSERT_TRUE(store.loadBook(Device::book(), ledger));
  ASSERT_TRUE(store.saveBook(ledger));
  ASSERT_TRUE(store.saveBook(ledger));
  const uint64_t before = fs.lookups();
  ASSERT_TRUE(store.loadBook(Device::book(), ledger));
  EXPECT_LE(fs.lookups() - before, 3u) << "one per slot and one to read the winner";
  const uint64_t afterLoad = fs.lookups();
  ASSERT_TRUE(store.saveBook(ledger));
  EXPECT_EQ(fs.lookups() - afterLoad, 1u) << "only the open of the slot it writes";
}

TEST(Store, AnnotationLedgerHoldsExactlyItsMaximumAndStaysLoadable) {
  MemFs fs;
  Store store(fs, testKey(), 6, testBox());
  ASSERT_TRUE(store.begin());
  std::vector<AnnotationRecord> records(Store::kMaxAnnotations);
  for (size_t i = 0; i < records.size(); ++i) records[i].key = i + 1;
  ASSERT_TRUE(store.saveAnnotations(Device::book(), records.data(), Store::kMaxAnnotations));
  AnnotationRecord* loaded = nullptr;
  uint16_t count = 0;
  ASSERT_TRUE(store.loadAnnotations(Device::book(), &loaded, &count));
  EXPECT_EQ(count, Store::kMaxAnnotations) << "a full ledger read as empty republishes the book at every exit";
  EXPECT_EQ(loaded[511].key, 512u);
  delete[] loaded;
}

TEST(Store, AnnotationLedgerRoundTrip) {
  MemFs fs;
  Store store(fs, testKey(), 6, testBox());
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
    Store store(fs, testKey(), 6, testBox());
    ASSERT_TRUE(store.begin());
    EXPECT_TRUE(store.creds().paired());
    EXPECT_STREQ(store.creds().token, kLegacyToken);
    EXPECT_STREQ(store.creds().deviceId, kSelfId);
    EXPECT_STREQ(store.creds().baseUrl, kDefaultBaseUrl);
    EXPECT_STREQ(store.creds().serverIp, "104.21.0.166");
    EXPECT_STREQ(store.creds().serverIpHost, "borges.runadev.com");
  }
  // The legacy file stays, so a rollback still works, and the import does not rerun.
  EXPECT_TRUE(fs.has(kLegacyCredentialPath));
  fs.put(kLegacyCredentialPath, "{\"deviceId\":\"x\",\"token\":\"other\"}");
  Store again(fs, testKey(), 6, testBox());
  ASSERT_TRUE(again.begin());
  EXPECT_STREQ(again.creds().token, kLegacyToken);
}

TEST(LegacyImport, TheOlderFileNameAndAPlainTokenWork) {
  MemFs fs;
  fs.put(kLegacyCredentialPathOld, std::string("{\"deviceId\":\"") + kSelfId + "\",\"token\":\"plain_token\"}");
  Store store(fs, testKey(), 6, testBox());
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
  Store store(fs, testKey(), 6, testBox());
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
    Store store(fs, testKey(), 6, testBox());
    ASSERT_TRUE(store.begin());
    EXPECT_FALSE(store.creds().paired()) << file.substr(0, 40);
  }
}

TEST(Store, ABaseUrlOnTheCardCannotMoveTheServer) {
  MemFs fs;
  {
    Store store(fs, testKey(), 6, testBox());
    ASSERT_TRUE(store.begin());
    copyText(store.creds().baseUrl, sizeof(store.creds().baseUrl), "https://borges.runadev.com@evil.example");
    copyText(store.creds().deviceId, sizeof(store.creds().deviceId), kSelfId);
    copyText(store.creds().token, sizeof(store.creds().token), "t");
    ASSERT_TRUE(store.saveCredentials());
  }
  Store again(fs, testKey(), 6, testBox());
  ASSERT_TRUE(again.begin());
  EXPECT_TRUE(again.creds().paired());
  EXPECT_STREQ(again.creds().baseUrl, kDefaultBaseUrl);
}

TEST(LegacyImport, ABaseUrlInTheOldFileIsNeverBelieved) {
  MemFs fs;
  fs.put(kLegacyCredentialPath, std::string("{\"baseUrl\":\"https://evil.example\",\"deviceId\":\"") + kSelfId + "\",\"token\":\"t\"}");
  Store store(fs, testKey(), 6, testBox());
  ASSERT_TRUE(store.begin());
  EXPECT_TRUE(store.creds().paired());
  EXPECT_STREQ(store.creds().baseUrl, kDefaultBaseUrl);
}
