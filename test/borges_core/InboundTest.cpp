// Highlights made elsewhere: what the pull keeps, within what bounds, and what a crash leaves.

#include <map>
#include <string>
#include <vector>

#include "Inbound.h"
#include "TestSupport.h"

using namespace borges;
using namespace borges_test;

namespace {

constexpr const char* kWebId = "33333333-3333-4333-8333-333333333333";

std::string uuidOf(int n) {
  char text[37];
  snprintf(text, sizeof(text), "aaaaaaaa-0000-3000-8000-%012d", n);
  return text;
}

struct Mark {
  uint64_t seq = 0;
  int id = 1;
  std::string origin = kOtherId;
  std::string type = "annotation.created";
  std::string text = "Un subrayado";
  std::string chapter = "Uno";
  std::string device = "Kobo";
  int revision = 1;
  std::string xpointer = "/body/DocFragment[3]/body/p[2]/text().0";
  std::string extraPayload;
  std::string directive = "apply_remote";
  std::string metadata;
  std::string md5 = kMd5A;
};

std::string eventJson(const Mark& m) {
  const bool del = m.type.find("deleted") != std::string::npos;
  std::string payload = "\"sync_id\":\"" + uuidOf(m.id) + "\"";
  if (!del) {
    payload += ",\"kind\":\"highlight\",\"text\":\"" + m.text + "\",\"chapter\":\"" + m.chapter + "\"";
    if (!m.xpointer.empty()) payload += ",\"xpointer\":\"" + m.xpointer + "\"";
    payload += m.extraPayload;
  }
  const std::string metadata =
      m.metadata.empty() ? "\"annotation_revision\":\"" + std::to_string(m.revision) + "\"" : m.metadata;
  return "{\"event_id\":\"evt-" + std::to_string(m.seq) + "\",\"server_sequence\":\"" + std::to_string(m.seq) +
         "\",\"origin_device\":{\"id\":\"" + m.origin + "\",\"name\":\"" + m.device + "\"},\"event_type\":\"" + m.type +
         "\",\"aggregate_type\":\"annotation\",\"aggregate_id\":\"" + uuidOf(m.id) +
         "\",\"book_identifier\":{\"kind\":\"koreader_partial_md5\",\"value\":\"" + m.md5 +
         "\"},\"occurred_at\":\"2026-11-01T10:00:00.000Z\",\"time_precision\":\"exact\",\"payload\":{" + payload +
         "},\"directive\":\"" + m.directive + "\",\"directive_metadata\":{" + metadata + "}}";
}

class InboundTest : public ::testing::Test {
 protected:
  void SetUp() override { d.boot(); }

  SessionReport manual() { return d.engine->runManualSession(nullptr, nullptr, nullptr); }

  void feed(const Mark& m) { d.server.addFeedEvent(m.seq, eventJson(m)); }

  std::vector<std::string> texts(IOwnPassages* own = nullptr, const char* md5 = kMd5A) {
    InboundList list(d.fs);
    EXPECT_TRUE(list.load(Device::book(md5), d.store->state().accountEpoch, own));
    std::vector<std::string> out;
    auto h = std::make_unique<InboundHighlight>();
    for (uint16_t i = 0; i < list.count(); ++i) {
      EXPECT_TRUE(list.read(i, *h));
      out.push_back(h->text);
    }
    return out;
  }

  std::string logFile(const char* md5 = kMd5A) const {
    return std::string(kInboundDir) + "/" + std::string(md5, 2) + "/" + (md5 + 2);
  }

  Device d;
};

using Strings = std::vector<std::string>;

class SamePassage : public IOwnPassages {
 public:
  void add(const char* text, int32_t spine) { own.push_back({passageHash(text), spine}); }
  bool isOwn(uint64_t passage, int32_t spine) override {
    for (const auto& o : own) {
      if (o.first == passage && (spine < 0 || o.second == spine)) return true;
    }
    return false;
  }
  std::vector<std::pair<uint64_t, int32_t>> own;
};

}  // namespace

// --- What is kept ------------------------------------------------------------------------

TEST_F(InboundTest, AHighlightFromElsewhereLandsWithAllARowNeeds) {
  Mark m;
  m.seq = 10;
  m.extraPayload = ",\"time\":1790000000000,\"percentage\":40.5";
  feed(m);
  EXPECT_EQ(manual().eventsApplied, 1);
  EXPECT_EQ(d.store->state().cursor, 10u);

  InboundList list(d.fs);
  ASSERT_TRUE(list.load(Device::book(), d.store->state().accountEpoch, nullptr));
  ASSERT_EQ(list.count(), 1);
  EXPECT_FALSE(list.partial());
  auto h = std::make_unique<InboundHighlight>();
  ASSERT_TRUE(list.read(0, *h));
  EXPECT_STREQ(h->text, "Un subrayado");
  EXPECT_STREQ(h->chapter, "Uno");
  EXPECT_STREQ(h->device, "Kobo");
  EXPECT_STREQ(h->xpointer, "/body/DocFragment[3]/body/p[2]/text().0");
  EXPECT_TRUE(h->hasPct);
  EXPECT_EQ(h->pctMicro, 40500000u);
  EXPECT_EQ(h->timeEpoch, 1790000000);
  EXPECT_EQ(h->revision, 1u);
  EXPECT_EQ(spineOfXpointer(h->xpointer), 2);
}

TEST_F(InboundTest, WhereAndWhenFallBackHonestly) {
  Mark byPage;
  byPage.seq = 10;
  byPage.id = 1;
  byPage.text = "por pagina";
  byPage.extraPayload = ",\"page\":25,\"total_pages\":100";
  feed(byPage);
  Mark nothing;
  nothing.seq = 11;
  nothing.id = 2;
  nothing.text = "sin lugar";
  nothing.xpointer = "";
  feed(nothing);
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);

  InboundList list(d.fs);
  ASSERT_TRUE(list.load(Device::book(), d.store->state().accountEpoch, nullptr));
  ASSERT_EQ(list.count(), 2);
  auto h = std::make_unique<InboundHighlight>();
  ASSERT_TRUE(list.read(0, *h));
  EXPECT_STREQ(h->text, "por pagina");
  EXPECT_TRUE(h->hasPct);
  EXPECT_EQ(h->pctMicro, 25000000u);
  // The event's own date, since this one was stated exact, and no "time" came.
  int64_t expected = 0;
  ASSERT_TRUE(parseRfc3339("2026-11-01T10:00:00Z", expected));
  EXPECT_EQ(h->timeEpoch, expected);
  ASSERT_TRUE(list.read(1, *h));
  EXPECT_STREQ(h->text, "sin lugar");
  EXPECT_FALSE(h->hasPct);
  EXPECT_EQ(h->xpointer[0], '\0');
}

TEST_F(InboundTest, AnUncertainClockIsNoDate) {
  std::string json = eventJson([] {
    Mark m;
    m.seq = 10;
    return m;
  }());
  const std::string exact = "\"time_precision\":\"exact\"";
  json.replace(json.find(exact), exact.size(), "\"time_precision\":\"approximate\"");
  d.server.addFeedEvent(10, json);
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  InboundList list(d.fs);
  ASSERT_TRUE(list.load(Device::book(), d.store->state().accountEpoch, nullptr));
  auto h = std::make_unique<InboundHighlight>();
  ASSERT_TRUE(list.read(0, *h));
  EXPECT_EQ(h->timeEpoch, 0);
}

TEST_F(InboundTest, RowsComeInBookOrderNotInTheOrderTheyWereMade) {
  const struct {
    double pct;
    const char* text;
  } marks[] = {{70, "setenta"}, {10, "diez"}, {40, "cuarenta"}};
  uint64_t seq = 10;
  int id = 1;
  for (const auto& mark : marks) {
    Mark m;
    m.seq = seq++;
    m.id = id++;
    m.text = mark.text;
    m.extraPayload = ",\"percentage\":" + std::to_string(mark.pct);
    feed(m);
  }
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(texts(), (Strings{"diez", "cuarenta", "setenta"}));
}

TEST_F(InboundTest, LongTextIsCutOnACharacterAndSaysSo) {
  std::string text;
  for (int i = 0; i < 3000; ++i) text += "\xC3\xB1";
  Mark m;
  m.seq = 10;
  m.text = text;
  feed(m);
  ASSERT_EQ(manual().eventsApplied, 1);
  InboundList list(d.fs);
  ASSERT_TRUE(list.load(Device::book(), d.store->state().accountEpoch, nullptr));
  auto h = std::make_unique<InboundHighlight>();
  ASSERT_TRUE(list.read(0, *h));
  EXPECT_TRUE(h->truncated);
  const size_t len = strlen(h->text);
  EXPECT_LT(len, kInboundTextMax);
  EXPECT_EQ(len % 2, 0u) << "half an n";
}

// --- Revision and delete -----------------------------------------------------------------

TEST_F(InboundTest, ANewerRevisionReplacesAndAnOlderOneDoesNot) {
  Mark m;
  m.seq = 10;
  m.text = "primera";
  m.revision = 3;
  feed(m);
  m.seq = 11;
  m.type = "annotation.updated";
  m.text = "segunda";
  m.revision = 4;
  feed(m);
  m.seq = 12;
  m.text = "vieja";
  m.revision = 2;  // Arrives later, says less.
  feed(m);
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(texts(), (Strings{"segunda"}));
}

TEST_F(InboundTest, AServerThatDoesNotSayTheRevisionLeavesItToTheOrderOfArrival) {
  Mark m;
  m.seq = 10;
  m.text = "primera";
  feed(m);
  m.seq = 11;
  m.type = "annotation.updated";
  m.text = "segunda";
  m.metadata = "\"metadata_omitted\":true";
  feed(m);
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(texts(), (Strings{"segunda"}));
}

TEST_F(InboundTest, ADeleteTakesItOffTheListAndOffTheCardBeforeTheCursorMoves) {
  Mark keep;
  keep.seq = 10;
  keep.id = 1;
  keep.text = "se queda";
  feed(keep);
  Mark gone;
  gone.seq = 11;
  gone.id = 2;
  gone.text = "palabrasquesevan";
  feed(gone);
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(texts().size(), 2u);
  EXPECT_NE(d.fs.content(logFile()).find("palabrasquesevan"), std::string::npos);

  Mark del;
  del.seq = 12;
  del.id = 2;
  del.type = "annotation.deleted";
  del.revision = 2;
  feed(del);
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(texts(), (Strings{"se queda"}));
  EXPECT_EQ(d.fs.content(logFile()).find("palabrasquesevan"), std::string::npos);
}

TEST_F(InboundTest, DeletingTheLastOneLeavesNoFile) {
  Mark m;
  m.seq = 10;
  feed(m);
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  ASSERT_TRUE(d.fs.has(logFile()));
  EXPECT_TRUE(InboundList::hasAny(d.fs, Device::book(), d.store->state().accountEpoch, nullptr));
  m.seq = 11;
  m.type = "annotation.deleted";
  m.revision = 2;
  feed(m);
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_FALSE(d.fs.has(logFile()));
  EXPECT_FALSE(InboundList::hasAny(d.fs, Device::book(), d.store->state().accountEpoch, nullptr));
}

TEST_F(InboundTest, ADeleteOfSomethingNeverSeenCostsNothing) {
  Mark m;
  m.seq = 10;
  m.type = "annotation.deleted";
  feed(m);
  ASSERT_EQ(manual().eventsApplied, 1);
  EXPECT_EQ(d.store->state().cursor, 10u);
  EXPECT_FALSE(d.fs.has(logFile()));
}

TEST_F(InboundTest, AHighlightDeletedAndMadeAgainIsBack) {
  Mark m;
  m.seq = 10;
  m.text = "de vuelta";
  feed(m);
  m.seq = 11;
  m.type = "annotation.deleted";
  m.revision = 2;
  feed(m);
  m.seq = 12;
  m.type = "annotation.created";
  m.revision = 3;
  feed(m);
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(texts(), (Strings{"de vuelta"}));
}

// --- What is not somebody else's highlight -------------------------------------------------

TEST_F(InboundTest, OwnEventsNeverComeBackAsInbound) {
  Mark own;
  own.seq = 10;
  own.origin = kSelfId;
  own.directive = "own";
  feed(own);
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(texts(), Strings{});
  EXPECT_FALSE(d.fs.has(logFile()));
}

TEST_F(InboundTest, AWebEditOfAHighlightThisDevicePublishedIsStillOurs) {
  uint8_t sync[16];
  ASSERT_TRUE(uuidParse(uuidOf(5).c_str(), sync));
  KnownAnnotation known{};
  known.key = 99;
  memcpy(known.syncId, sync, 16);
  ASSERT_TRUE(d.store->addKnownAnnotations(Device::book(), &known, 1, false));

  Mark web;
  web.seq = 10;
  web.id = 5;
  web.origin = kWebId;
  web.device = "Web";
  web.type = "annotation.updated";
  web.revision = 7;
  feed(web);
  ASSERT_EQ(manual().eventsApplied, 1);
  EXPECT_EQ(texts(), Strings{});
  // And its revision is noted, which is what a delete from here needs.
  AnnotationRecord* records = nullptr;
  uint16_t count = 0;
  ASSERT_TRUE(d.store->loadAnnotations(Device::book(), &records, &count));
  ASSERT_EQ(count, 1);
  EXPECT_EQ(records[0].revision, 7u);
  delete[] records;
}

TEST_F(InboundTest, BookmarksConflictsAndOtherDirectivesAreNotListed) {
  Mark bookmark;
  bookmark.seq = 10;
  bookmark.id = 1;
  bookmark.type = "bookmark.upserted";
  feed(bookmark);
  Mark kindOnly;
  kindOnly.seq = 11;
  kindOnly.id = 2;
  kindOnly.extraPayload = ",\"kind\":\"bookmark\"";
  feed(kindOnly);
  Mark conflict;
  conflict.seq = 12;
  conflict.id = 3;
  conflict.directive = "annotation_conflict";
  conflict.metadata = "\"annotation_conflict\":true";
  feed(conflict);
  Mark keepLocal;
  keepLocal.seq = 13;
  keepLocal.id = 4;
  keepLocal.directive = "keep_local";
  feed(keepLocal);
  Mark noId;
  noId.seq = 14;
  const std::string good = uuidOf(1);
  std::string json = eventJson(noId);
  size_t at;
  while ((at = json.find(good)) != std::string::npos) json.replace(at, good.size(), "s");
  d.server.addFeedEvent(14, json);
  Mark empty;
  empty.seq = 15;
  empty.id = 5;
  empty.text = "   ";
  feed(empty);
  EXPECT_EQ(manual().eventsApplied, 6);
  EXPECT_EQ(d.store->state().cursor, 15u);
  EXPECT_FALSE(d.fs.has(logFile()));
}

TEST_F(InboundTest, ThePassageOnBothSidesShowsOnceAsItsOwn) {
  Mark same;
  same.seq = 10;
  same.id = 1;
  same.text = "Mismo\\u00a0  pasaje\\n aqui";
  feed(same);
  Mark other;
  other.seq = 11;
  other.id = 2;
  other.text = "Otro pasaje";
  feed(other);
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);

  SamePassage mine;
  mine.add("Mismo pasaje aqui", 2);  // The spine the xpointer of those two names.
  EXPECT_EQ(texts(&mine), (Strings{"Otro pasaje"}));
  EXPECT_TRUE(InboundList::hasAny(d.fs, Device::book(), d.store->state().accountEpoch, &mine));
  mine.add("Otro pasaje", 2);
  EXPECT_FALSE(InboundList::hasAny(d.fs, Device::book(), d.store->state().accountEpoch, &mine));

  SamePassage elsewhere;
  elsewhere.add("Mismo pasaje aqui", 7);  // The same words in another chapter are another passage.
  EXPECT_EQ(texts(&elsewhere).size(), 2u);
}

TEST_F(InboundTest, TwoOtherDevicesWithThePassageShowOnce) {
  Mark a;
  a.seq = 10;
  a.id = 1;
  a.device = "Kobo";
  a.text = "Mismo pasaje";
  feed(a);
  Mark b;
  b.seq = 11;
  b.id = 2;
  b.device = "Telefono";
  b.text = "Mismo  pasaje";
  feed(b);
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  InboundList list(d.fs);
  ASSERT_TRUE(list.load(Device::book(), d.store->state().accountEpoch, nullptr));
  ASSERT_EQ(list.count(), 1);
  auto h = std::make_unique<InboundHighlight>();
  ASSERT_TRUE(list.read(0, *h));
  EXPECT_STREQ(h->device, "Telefono");
}

TEST(InboundPassage, ComparesAsThePluginDoes) {
  EXPECT_EQ(passageHash("uno dos"), passageHash("  uno\n\t dos  "));
  EXPECT_EQ(passageHash("uno dos"), passageHash("uno\xC2\xA0"
                                                "dos"));
  EXPECT_EQ(passageHash("palabra"), passageHash("pala\xC2\xAD"
                                                "bra"));
  EXPECT_NE(passageHash("uno dos"), passageHash("uno  tres"));
  EXPECT_EQ(passageHash(" \n "), 0u);
  EXPECT_EQ(passageHash(nullptr), 0u);
  // What a stored record kept and the whole text agree.
  const std::string whole(2000, 'x');
  EXPECT_EQ(passageHash(whole.c_str()), passageHash(whole.substr(0, kInboundTextMax - 1).c_str()));
}

TEST(InboundPassage, TheSpineAnXpointerNames) {
  EXPECT_EQ(spineOfXpointer("/body/DocFragment[1]/body/p[2]/text().0"), 0);
  EXPECT_EQ(spineOfXpointer("/body/DocFragment[12]/body"), 11);
  EXPECT_EQ(spineOfXpointer("/body/DocFragment/body/p"), 0);
  EXPECT_EQ(spineOfXpointer("/body/DocFragment[0]/body"), -1);
  EXPECT_EQ(spineOfXpointer("/body/DocFragment[x]/body"), -1);
  EXPECT_EQ(spineOfXpointer("/body/p[1]"), -1);
  EXPECT_EQ(spineOfXpointer(""), -1);
  EXPECT_EQ(spineOfXpointer(nullptr), -1);
}

// --- Replays -----------------------------------------------------------------------------

TEST_F(InboundTest, AReplayChangesNothing) {
  for (uint64_t seq = 10; seq < 14; ++seq) {
    Mark m;
    m.seq = seq;
    m.id = static_cast<int>(seq);
    m.text = "texto " + std::to_string(seq);
    feed(m);
  }
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  const std::string before = d.fs.content(logFile());
  const Strings listed = texts();
  ASSERT_EQ(listed.size(), 4u);

  d.store->state().cursor = 0;
  d.store->state().committedCursor = 0;
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.fs.content(logFile()), before);
  EXPECT_EQ(texts(), listed);
}

TEST_F(InboundTest, AReplayOfAHighlightDeletedSinceDoesNotBringItBack) {
  Mark m;
  m.seq = 10;
  m.text = "se fue";
  feed(m);
  m.seq = 11;
  m.type = "annotation.deleted";
  m.revision = 2;
  feed(m);
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  ASSERT_EQ(texts(), Strings{});
  d.store->state().cursor = 0;
  d.store->state().committedCursor = 0;
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(texts(), Strings{});
  EXPECT_FALSE(d.fs.has(logFile()));
}

// --- Bounds ------------------------------------------------------------------------------

namespace {

InboundHighlight* makeHighlight(uint64_t seq, int id, const std::string& text) {
  auto* h = new InboundHighlight;
  h->serverSeq = seq;
  h->revision = 1;
  uuidParse(uuidOf(id).c_str(), h->syncId);
  copyText(h->text, sizeof(h->text), text.c_str());
  h->passage = passageHash(h->text);
  h->hasPct = true;
  h->pctMicro = static_cast<uint32_t>(id);
  return h;
}

// Which book, by number, with a hash nothing else in these tests uses.
BookId bookNumber(int n) {
  char hex[33];
  snprintf(hex, sizeof(hex), "%032x", n + 0x1000);
  BookId id;
  id.setHex(hex);
  return id;
}

}  // namespace

TEST_F(InboundTest, ABookNeverHoldsMoreThanItsBoundsAndKeepsTheNewest) {
  InboundStore store(d.fs);
  const uint16_t epoch = d.store->state().accountEpoch;
  const int total = kInboundMaxRecords * 3;
  for (int i = 1; i <= total; ++i) {
    std::unique_ptr<InboundHighlight> h(
        makeHighlight(static_cast<uint64_t>(i), i, "texto numero " + std::to_string(i)));
    ASSERT_TRUE(store.apply(Device::book(), epoch, *h));
    ASSERT_LE(d.fs.content(logFile()).size(), kInboundMaxBookBytes);
  }
  ASSERT_TRUE(store.finish());
  EXPECT_LE(d.fs.peakOpen(), 3);

  InboundList list(d.fs);
  ASSERT_TRUE(list.load(Device::book(), epoch, nullptr));
  EXPECT_TRUE(list.partial());
  EXPECT_LE(list.count(), kInboundMaxRecords);
  EXPECT_GE(list.count(), kInboundMaxRecords / 4 * 3);
  auto h = std::make_unique<InboundHighlight>();
  ASSERT_TRUE(list.read(list.count() - 1, *h));  // pctMicro follows the id here.
  EXPECT_EQ(std::string(h->text), "texto numero " + std::to_string(total));
}

TEST_F(InboundTest, BigTextsReachTheByteBoundFirst) {
  InboundStore store(d.fs);
  const uint16_t epoch = d.store->state().accountEpoch;
  const std::string big(kInboundTextMax - 16, 'b');
  for (int i = 1; i <= 240; ++i) {
    std::unique_ptr<InboundHighlight> h(makeHighlight(static_cast<uint64_t>(i), i, std::to_string(i) + big));
    copyText(h->xpointer, sizeof(h->xpointer), std::string(kMaxXpointer - 1, 'x').c_str());
    ASSERT_TRUE(store.apply(Device::book(), epoch, *h));
    ASSERT_LE(d.fs.content(logFile()).size(), kInboundMaxBookBytes);
  }
  ASSERT_TRUE(store.finish());
  InboundList list(d.fs);
  ASSERT_TRUE(list.load(Device::book(), epoch, nullptr));
  EXPECT_TRUE(list.partial());
  EXPECT_LT(list.count(), 70);
}

TEST_F(InboundTest, TheCardHoldsNoMoreThanItsTotalAndTheBookUsedLongestAgoGoes) {
  InboundStore store(d.fs);
  const uint16_t epoch = d.store->state().accountEpoch;
  const std::string big(kInboundTextMax - 16, 'b');
  uint64_t seq = 1;
  const int perBook = 120;
  const int books = static_cast<int>(kInboundMaxTotalBytes / (perBook * 540)) + 6;
  ASSERT_LT(books, kInboundMaxBooks);
  for (int b = 0; b < books; ++b) {
    for (int i = 1; i <= perBook; ++i) {
      std::unique_ptr<InboundHighlight> h(makeHighlight(seq++, i, std::to_string(i) + big));
      ASSERT_TRUE(store.apply(bookNumber(b), epoch, *h));
    }
  }
  ASSERT_TRUE(store.finish());

  size_t onCard = 0;
  for (const std::string& path : d.fs.paths()) {
    if (path.rfind(kInboundDir, 0) == 0 && path.find("/index") == std::string::npos)
      onCard += d.fs.content(path).size();
  }
  EXPECT_LE(onCard, kInboundMaxTotalBytes);
  EXPECT_GT(onCard, kInboundMaxTotalBytes / 2);
  EXPECT_FALSE(InboundList::hasAny(d.fs, bookNumber(0), epoch, nullptr)) << "the oldest went";
  EXPECT_TRUE(InboundList::hasAny(d.fs, bookNumber(books - 1), epoch, nullptr)) << "the newest stays";
}

TEST_F(InboundTest, TheCardNamesNoMoreThanItsBooks) {
  InboundStore store(d.fs);
  const uint16_t epoch = d.store->state().accountEpoch;
  uint64_t seq = 1;
  for (int b = 0; b < kInboundMaxBooks + 10; ++b) {
    std::unique_ptr<InboundHighlight> h(makeHighlight(seq++, 1, "texto"));
    ASSERT_TRUE(store.apply(bookNumber(b), epoch, *h));
  }
  ASSERT_TRUE(store.finish());
  int logs = 0;
  for (const std::string& path : d.fs.paths()) {
    if (path.rfind(kInboundDir, 0) == 0 && path.find("/index") == std::string::npos) ++logs;
  }
  EXPECT_LE(logs, kInboundMaxBooks);
  EXPECT_FALSE(InboundList::hasAny(d.fs, bookNumber(0), epoch, nullptr));
  EXPECT_TRUE(InboundList::hasAny(d.fs, bookNumber(kInboundMaxBooks + 9), epoch, nullptr));
}

TEST_F(InboundTest, OnlyAScreenOrAPullPaysForAnyOfThis) {
  // What a finalize with a book loaded allocates did not grow by a highlight's text.
  EXPECT_LT(sizeof(RemoteEvent), 1024u);
  EXPECT_LT(sizeof(InboundHighlight), 1300u);
  EXPECT_LT(sizeof(InboundStore), 400u);
  EXPECT_LT(sizeof(RemoteEvent) + sizeof(BookLedger) + sizeof(Batch), 4u * 1024);
}

TEST_F(InboundTest, NoFileIsLeftOpenEvenThroughACompaction) {
  InboundStore store(d.fs);
  const uint16_t epoch = d.store->state().accountEpoch;
  for (int i = 1; i <= kInboundMaxRecords + 2; ++i) {
    std::unique_ptr<InboundHighlight> h(makeHighlight(static_cast<uint64_t>(i), i, "texto"));
    ASSERT_TRUE(store.apply(Device::book(), epoch, *h));
  }
  ASSERT_TRUE(store.finish());
  EXPECT_LE(d.fs.peakOpen(), 3) << "the spool makes four, the glue's pool";
}

// --- Accounts ----------------------------------------------------------------------------

TEST_F(InboundTest, AnotherAccountFindsNothingOfTheFirstNeitherOnTheListNorOnTheCard) {
  ASSERT_TRUE(d.store->adoptAccount("cuenta-uno", "uno", *d.outbox));
  ASSERT_TRUE(d.store->saveCredentials());
  Mark m;
  m.seq = 10;
  m.text = "palabrasdelprimero";
  feed(m);
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  const uint16_t first = d.store->state().accountEpoch;
  ASSERT_TRUE(InboundList::hasAny(d.fs, Device::book(), first, nullptr));

  ASSERT_TRUE(d.store->adoptAccount("cuenta-uno", "uno", *d.outbox));
  EXPECT_TRUE(InboundList::hasAny(d.fs, Device::book(), d.store->state().accountEpoch, nullptr))
      << "the same one keeps them";

  ASSERT_TRUE(d.store->adoptAccount("cuenta-dos", "dos", *d.outbox));
  EXPECT_NE(d.store->state().accountEpoch, first);
  EXPECT_FALSE(InboundList::hasAny(d.fs, Device::book(), d.store->state().accountEpoch, nullptr));
  for (const std::string& path : d.fs.paths()) {
    if (path != kSpoolPath) EXPECT_EQ(d.fs.content(path).find("palabrasdelprimero"), std::string::npos) << path;
  }
}

TEST_F(InboundTest, ALogOfAnotherEpochReadsAsBlankEvenIfThePurgeNeverHappened) {
  InboundStore store(d.fs);
  std::unique_ptr<InboundHighlight> h(makeHighlight(50, 1, "de antes"));
  ASSERT_TRUE(store.apply(Device::book(), 7, *h));
  ASSERT_TRUE(store.finish());
  EXPECT_TRUE(InboundList::hasAny(d.fs, Device::book(), 7, nullptr));
  EXPECT_FALSE(InboundList::hasAny(d.fs, Device::book(), 8, nullptr));

  // Writing under the new one replaces it, and a lower sequence is no replay here.
  InboundStore next(d.fs);
  std::unique_ptr<InboundHighlight> h2(makeHighlight(5, 2, "de ahora"));
  ASSERT_TRUE(next.apply(Device::book(), 8, *h2));
  ASSERT_TRUE(next.finish());
  InboundList list(d.fs);
  ASSERT_TRUE(list.load(Device::book(), 8, nullptr));
  ASSERT_EQ(list.count(), 1);
  auto out = std::make_unique<InboundHighlight>();
  ASSERT_TRUE(list.read(0, *out));
  EXPECT_STREQ(out->text, "de ahora");
  EXPECT_EQ(d.fs.content(logFile()).find("de antes"), std::string::npos);
}

// --- Crashes -----------------------------------------------------------------------------

TEST_F(InboundTest, BytesATornWriteLeftAreSkippedAndGoAtTheNextWrite) {
  Mark m;
  m.seq = 10;
  m.id = 1;
  m.text = "antes";
  feed(m);
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  // A frame cut short, as a crash leaves it: the magic and a length that promises more.
  std::string log = d.fs.content(logFile());
  const std::string good = log;
  log.append("\xB1\x57\x40\x00\x01\x02\x03\x04torn", 12);
  d.fs.put(logFile(), log);
  EXPECT_EQ(texts(), (Strings{"antes"}));

  m.seq = 11;
  m.id = 2;
  m.text = "despues";
  feed(m);
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(texts(), (Strings{"antes", "despues"}));
  EXPECT_EQ(d.fs.content(logFile()).find("torn"), std::string::npos);
}

TEST_F(InboundTest, ALogTornInItsHeaderIsNoLogAndNothingBreaks) {
  d.fs.mkdir((std::string(kInboundDir) + "/ab").c_str());
  d.fs.put(logFile(), "BIN");
  EXPECT_EQ(texts(), Strings{});
  Mark m;
  m.seq = 10;
  feed(m);
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(texts(), (Strings{"Un subrayado"}));
}

TEST_F(InboundTest, ACompactionACrashFinishedWinsAndOneItDidNotGoes) {
  Mark m;
  m.seq = 10;
  m.text = "uno";
  feed(m);
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  const std::string aside = logFile() + ".n";

  // Half written: what is there stays, and the half goes.
  d.fs.put(aside, "BIN1half");
  EXPECT_EQ(texts(), (Strings{"uno"}));
  EXPECT_FALSE(d.fs.has(aside));

  // Whole, with its seal: it wins, as when a crash came between removing the old and renaming.
  Mark other;
  other.seq = 11;
  other.id = 2;
  other.text = "dos";
  feed(other);
  Mark del;
  del.seq = 12;
  del.id = 1;
  del.type = "annotation.deleted";
  del.revision = 2;
  feed(del);
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  const std::string sealed = d.fs.content(logFile());
  ASSERT_EQ(texts(), (Strings{"dos"}));
  d.fs.put(aside, sealed);
  d.fs.put(logFile(), "stale");
  EXPECT_EQ(texts(), (Strings{"dos"}));
  EXPECT_FALSE(d.fs.has(aside));
  EXPECT_EQ(d.fs.content(logFile()), sealed);
}

// Every operation a pull does, a crash at each: what the cursor says was applied is on the card,
// the list ends up the same whatever happened, and it never holds anything twice.
TEST(InboundCrashMatrix, AtEveryWriteThePullConverges) {
  const Strings expected{"cero", "dos cambiado", "cuatro"};
  for (TornMode mode : {TornMode::Drop, TornMode::Half, TornMode::Keep}) {
    for (uint64_t kill = 1; kill < 400; ++kill) {
      Device d;
      d.fs.setTornMode(mode);
      d.config.pullLimit = 2;
      d.boot();
      const auto mark = [&](uint64_t seq, int id, const char* type, const char* text, int revision, int pct) {
        Mark m;
        m.seq = seq;
        m.id = id;
        m.type = type;
        m.text = text;
        m.revision = revision;
        m.extraPayload = ",\"percentage\":" + std::to_string(pct);
        d.server.addFeedEvent(seq, eventJson(m));
      };
      mark(10, 0, "annotation.created", "cero", 1, 0);
      mark(11, 1, "annotation.created", "uno", 1, 10);
      mark(12, 2, "annotation.created", "dos", 1, 20);
      mark(13, 1, "annotation.deleted", "", 2, 0);
      mark(14, 2, "annotation.updated", "dos cambiado", 2, 20);
      mark(15, 4, "annotation.created", "cuatro", 1, 40);
      mark(16, 3, "annotation.deleted", "", 1, 0);

      d.fs.killAtOp(d.fs.ops() + kill);
      d.engine->runManualSession(nullptr, nullptr, nullptr);
      const bool crashed = d.fs.dead();
      d.fs.killAtOp(0);
      if (crashed) d.crashAndBoot();
      for (int i = 0; i < 6; ++i) d.engine->runManualSession(nullptr, nullptr, nullptr);
      ASSERT_EQ(d.store->state().cursor, 16u) << "kill=" << kill;

      InboundList list(d.fs);
      ASSERT_TRUE(list.load(Device::book(), d.store->state().accountEpoch, nullptr)) << "kill=" << kill;
      Strings out;
      auto h = std::make_unique<InboundHighlight>();
      for (uint16_t i = 0; i < list.count(); ++i) {
        ASSERT_TRUE(list.read(i, *h)) << "kill=" << kill;
        out.push_back(h->text);
      }
      EXPECT_EQ(out, expected) << "kill=" << kill << " mode=" << static_cast<int>(mode);
      if (!crashed) break;
    }
  }
}

// Compaction is the part that rewrites: a crash anywhere in it loses nothing that was live.
TEST(InboundCrashMatrix, ACrashInACompactionLosesNothing) {
  for (TornMode mode : {TornMode::Drop, TornMode::Half, TornMode::Keep}) {
    for (uint64_t kill = 1; kill < 120; ++kill) {
      Device d;
      d.fs.setTornMode(mode);
      d.boot();
      const uint16_t epoch = d.store->state().accountEpoch;
      const BookId book = Device::book();
      {
        InboundStore store(d.fs);
        for (int i = 1; i <= 40; ++i) {
          std::unique_ptr<InboundHighlight> h(makeHighlight(static_cast<uint64_t>(i), i, "texto " + std::to_string(i)));
          ASSERT_TRUE(store.apply(book, epoch, *h));
        }
        ASSERT_TRUE(store.finish());
      }
      d.fs.killAtOp(d.fs.ops() + kill);
      bool ok = false;
      {
        // A delete compacts at finish: all of it runs under the kill.
        InboundStore store(d.fs);
        auto del = std::make_unique<InboundHighlight>();
        del->serverSeq = 41;
        del->revision = 2;
        del->deleted = true;
        uuidParse(uuidOf(40).c_str(), del->syncId);
        ok = store.apply(book, epoch, *del) && store.finish();
      }
      const bool crashed = d.fs.dead();
      d.fs.killAtOp(0);
      if (crashed) d.crashAndBoot();
      InboundList list(d.fs);
      ASSERT_TRUE(list.load(book, d.store->state().accountEpoch, nullptr)) << "kill=" << kill;
      // Either the delete landed or it did not, and the server would send it again.
      EXPECT_TRUE(list.count() == 39 || list.count() == 40) << "kill=" << kill << " count=" << list.count();
      auto h = std::make_unique<InboundHighlight>();
      for (uint16_t i = 0; i < list.count(); ++i) {
        ASSERT_TRUE(list.read(i, *h)) << "kill=" << kill << " row=" << i;
        ASSERT_EQ(std::string(h->text), "texto " + std::to_string(i + 1)) << "kill=" << kill;
      }
      if (!crashed) {
        EXPECT_TRUE(ok);
        EXPECT_EQ(list.count(), 39);
        break;
      }
    }
  }
}
