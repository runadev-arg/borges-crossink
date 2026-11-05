#include "Events.h"
#include "TestSupport.h"

using namespace borges;
using namespace borges_test;

namespace {

template <typename Fn>
std::string render(Fn fn) {
  JsonWriter counter(nullptr, 0);
  fn(counter);
  std::string buffer(counter.size(), '\0');
  JsonWriter w(buffer.data(), buffer.size());
  fn(w);
  EXPECT_TRUE(w.ok());
  return "{" + buffer + "}";
}

JsonDocument parse(const std::string& json) {
  JsonDocument doc;
  EXPECT_FALSE(deserializeJson(doc, json)) << json;
  return doc;
}

EventTime exactTime() {
  EventTime t;
  t.wall = WallTime{1784880000, TimePrecision::Exact};
  t.hasMonotonic = true;
  t.monotonicMs = 482100;
  return t;
}

}  // namespace

TEST(Events, ProgressMatchesTheProtocolExample) {
  Position p;
  p.setPercent(60);
  p.page = 60;
  p.totalPages = 100;
  copyText(p.xpointer, sizeof(p.xpointer), "/body/p[60].0");
  const BookId book = Device::book();
  auto doc = parse(render([&](JsonWriter& w) { events::writeProgress(w, book, exactTime(), p, false); }));
  EXPECT_EQ(doc["event_version"], 1);
  EXPECT_EQ(doc["event_type"], "progress.changed");
  EXPECT_EQ(doc["aggregate_type"], "reading_progress");
  EXPECT_EQ(doc["aggregate_id"], std::string("koreader_partial_md5:") + kMd5A);
  EXPECT_TRUE(doc["document_id"].isNull());
  EXPECT_FALSE(doc["document_id"].isUnbound());
  EXPECT_EQ(doc["book_identifier"]["kind"], "koreader_partial_md5");
  EXPECT_EQ(doc["book_identifier"]["value"], kMd5A);
  EXPECT_EQ(doc["occurred_at"], "2026-07-24T08:00:00Z");
  EXPECT_EQ(doc["time_precision"], "exact");
  EXPECT_EQ(doc["monotonic_ms"], "482100");
  EXPECT_EQ(doc["payload"]["percentage"], 60);
  EXPECT_EQ(doc["payload"]["current_page"], 60);
  EXPECT_EQ(doc["payload"]["total_pages"], 100);
  EXPECT_EQ(doc["payload"]["xpointer"], "/body/p[60].0");
  EXPECT_EQ(doc["payload"]["explicit_jump"], false);
}

TEST(Events, ProgressOmitsWhatTheServerWouldRefuse) {
  Position p;
  p.setPercent(100);
  p.page = 300;  // current_page > total_pages is progress_page_invalid.
  p.totalPages = 200;
  auto doc = parse(render([&](JsonWriter& w) { events::writeProgress(w, Device::book(), exactTime(), p, true); }));
  EXPECT_TRUE(doc["payload"]["current_page"].isUnbound());
  EXPECT_TRUE(doc["payload"]["total_pages"].isUnbound());
  EXPECT_TRUE(doc["payload"]["xpointer"].isUnbound());
  EXPECT_EQ(doc["payload"]["explicit_jump"], true);
}

TEST(Events, UnknownClockHasNoDate) {
  Position p;
  p.setPercent(1);
  EventTime t;
  t.wall = WallTime{1784880000, TimePrecision::Unknown};
  auto doc = parse(render([&](JsonWriter& w) { events::writeProgress(w, Device::book(), t, p, false); }));
  EXPECT_TRUE(doc["occurred_at"].isNull());
  EXPECT_EQ(doc["time_precision"], "unknown");
  EXPECT_TRUE(doc["monotonic_ms"].isUnbound());
}

TEST(Events, AnnotationHasAllTheFieldsAndDefaultsToGray) {
  ClipKey key;
  key.spine = 3;
  key.textOffset = 120;
  key.textHash = 999;
  ClipDetail detail;
  detail.text = "un \"laberinto\"\nde palabras";
  detail.chapter = "Tl\xC3\xB6n";
  detail.xpointer = "/body/p[3].10";
  detail.page = 12;
  detail.hasPct = true;
  detail.pctMicro = 12345678;
  detail.timeMs = 1784880000000;
  uint8_t syncId[16];
  events::deriveSyncId(Device::book(), key, syncId);
  char syncText[37];
  uuidFormat(syncId, syncText);
  bool valid = true;
  auto doc = parse(render([&](JsonWriter& w) {
    valid = events::writeAnnotation(w, Device::bookRef(), exactTime(), key, detail, syncId) && valid;
  }));
  EXPECT_TRUE(valid);
  EXPECT_TRUE(isCanonicalUuid(syncText));
  EXPECT_EQ(doc["event_type"], "annotation.created");
  EXPECT_EQ(doc["aggregate_type"], "annotation");
  EXPECT_EQ(doc["aggregate_id"], syncText);
  JsonObject payload = doc["payload"];
  EXPECT_EQ(payload["sync_id"], syncText);
  EXPECT_EQ(payload["text"], "un \"laberinto\"\nde palabras");
  EXPECT_EQ(payload["color"], "gray");
  EXPECT_EQ(payload["drawer"], "lighten");
  EXPECT_EQ(payload["chapter"], "Tl\xC3\xB6n");
  EXPECT_EQ(payload["xpointer"], "/body/p[3].10");
  EXPECT_EQ(payload["page"], 12);
  EXPECT_EQ(payload["time"], 1784880000000);
  EXPECT_EQ(payload["book"]["title"], "Ficciones");
  EXPECT_EQ(payload["book"]["author"], "Borges");
  EXPECT_EQ(payload["book"]["file"], "ficciones.epub");
  EXPECT_EQ(payload["book"]["total_pages"], 200);
  EXPECT_TRUE(payload["note"].isUnbound());
}

TEST(Events, AHighlightCarriesBothEndsSoItCrossesIntoKoreader) {
  ClipKey key;
  ClipDetail detail;
  detail.text = "texto";
  detail.xpointer = "/body/p[3]/text().10";
  detail.xpointerEnd = "/body/p[3]/text().15";
  uint8_t syncId[16] = {0};
  auto doc = parse(render([&](JsonWriter& w) { events::writeAnnotation(w, Device::bookRef(), exactTime(), key, detail, syncId); }));
  EXPECT_EQ(doc["payload"]["xpointer"], "/body/p[3]/text().10");
  EXPECT_EQ(doc["payload"]["position"]["pos0"], "/body/p[3]/text().10");
  EXPECT_EQ(doc["payload"]["position"]["pos1"], "/body/p[3]/text().15");
  detail.xpointerEnd = nullptr;
  auto single = parse(render([&](JsonWriter& w) { events::writeAnnotation(w, Device::bookRef(), exactTime(), key, detail, syncId); }));
  EXPECT_TRUE(single["payload"]["position"].isUnbound());
}

TEST(Events, ThePageIsTheBookWideOneWorkedOutFromThePercent) {
  ClipKey key;
  ClipDetail detail;
  detail.text = "texto";
  detail.hasPct = true;
  detail.pctMicro = 37500000;  // 37.5% of 200.
  uint8_t syncId[16] = {0};
  auto doc = parse(render([&](JsonWriter& w) { events::writeAnnotation(w, Device::bookRef(), exactTime(), key, detail, syncId); }));
  EXPECT_EQ(doc["payload"]["page"], 75);
  EXPECT_EQ(events::referencePage(0, 200), 1);
  EXPECT_EQ(events::referencePage(100000000, 200), 200);
  EXPECT_EQ(events::referencePage(100000001, 200), 200);
  EXPECT_EQ(events::referencePage(50000000, 0), -1);
}

TEST(Events, ABookmarkAlwaysHasAPageAndText) {
  ClipKey key;
  key.bookmark = true;
  ClipDetail detail;
  detail.text = "";
  detail.hasPct = true;
  detail.pctMicro = 10000000;
  uint8_t syncId[16] = {0};
  auto doc = parse(render([&](JsonWriter& w) { EXPECT_TRUE(events::writeAnnotation(w, Device::bookRef(), exactTime(), key, detail, syncId)); }));
  EXPECT_EQ(doc["payload"]["page"], 20);
  EXPECT_EQ(doc["payload"]["text"], "Bookmark - page 20");
}

TEST(Events, TextIsNormalizedAsKoreaderDoes) {
  ClipKey key;
  ClipDetail detail;
  detail.text = "uno\r\ndos\rtres\n\"cuatro\"";
  detail.page = 1;
  uint8_t syncId[16] = {0};
  auto doc = parse(render([&](JsonWriter& w) { events::writeAnnotation(w, Device::bookRef(), exactTime(), key, detail, syncId); }));
  EXPECT_EQ(doc["payload"]["text"], "uno\ndos\ntres\n\"cuatro\"");
}

TEST(Events, DeleteCarriesTheRevisionItLastSaw) {
  uint8_t syncId[16] = {0};
  auto doc = parse(render([&](JsonWriter& w) { events::writeAnnotationDeleted(w, Device::bookRef(), exactTime(), false, syncId, 7); }));
  EXPECT_EQ(doc["payload"]["base_revision"], "7");
}

TEST(Events, BookmarksUseTheirOwnEventTypes) {
  ClipKey key;
  key.bookmark = true;
  ClipDetail detail;
  detail.text = "marca";
  detail.page = 2;
  uint8_t syncId[16] = {0};
  auto doc = parse(render([&](JsonWriter& w) { events::writeAnnotation(w, Device::bookRef(), exactTime(), key, detail, syncId); }));
  EXPECT_EQ(doc["event_type"], "bookmark.upserted");
  EXPECT_EQ(doc["payload"]["drawer"], "bookmark");
  EXPECT_EQ(doc["payload"]["kind"], "bookmark");
  auto del = parse(render([&](JsonWriter& w) { events::writeAnnotationDeleted(w, Device::bookRef(), exactTime(), true, syncId, 0); }));
  EXPECT_EQ(del["event_type"], "bookmark.deleted");
}

// Found by the contract tests: delete a highlight, highlight the same passage again, and
// the server kept the tombstone and opened a conflict.
TEST(Events, AnnotationsCarryABaseRevisionAsKOReaderDoes) {
  ClipKey key;
  ClipDetail detail;
  detail.text = "t";
  detail.page = 1;
  uint8_t syncId[16] = {1};
  auto created = parse(render([&](JsonWriter& w) { events::writeAnnotation(w, Device::bookRef(), exactTime(), key, detail, syncId); }));
  EXPECT_STREQ(created["payload"]["base_revision"] | "", "0");
  auto deleted = parse(render([&](JsonWriter& w) { events::writeAnnotationDeleted(w, Device::bookRef(), exactTime(), false, syncId, 0); }));
  EXPECT_STREQ(deleted["payload"]["base_revision"] | "", "0");
}

TEST(Events, AnnotationThatCannotBeValidIsRefused) {
  ClipKey key;
  ClipDetail noText;
  noText.page = 1;
  noText.text = "   \n";
  uint8_t syncId[16] = {0};
  JsonWriter w(nullptr, 0);
  EXPECT_FALSE(events::writeAnnotation(w, Device::bookRef(), exactTime(), key, noText, syncId));
  ClipDetail noLocator;
  noLocator.text = "texto";
  EXPECT_FALSE(events::writeAnnotation(w, Device::bookRef(), exactTime(), key, noLocator, syncId));
}

TEST(Events, SyncIdIsDerivedFromTheKeyAndStable) {
  ClipKey a;
  a.spine = 1;
  a.textOffset = 2;
  a.textHash = 3;
  ClipKey b = a;
  b.textOffset = 4;
  uint8_t x[16], y[16], z[16];
  events::deriveSyncId(Device::book(), a, x);
  events::deriveSyncId(Device::book(), a, y);
  events::deriveSyncId(Device::book(), b, z);
  EXPECT_EQ(memcmp(x, y, 16), 0);
  EXPECT_NE(memcmp(x, z, 16), 0);
  events::deriveSyncId(Device::book(kMd5B), a, z);
  EXPECT_NE(memcmp(x, z, 16), 0);
  EXPECT_NE(events::clipLedgerKey(Device::book(), a), events::clipLedgerKey(Device::book(), b));
}

TEST(Events, SessionWithinRulesAndNotOutside) {
  SessionSummary s;
  s.started = WallTime{1784880000, TimePrecision::Exact};
  s.ended = WallTime{1784880600, TimePrecision::Anchored};
  s.pagesRead = 12;
  s.startPage = 40;
  s.endPage = 52;
  bool valid = false;
  auto doc = parse(render([&](JsonWriter& w) { valid = events::writeSession(w, Device::book(), s); }));
  EXPECT_TRUE(valid);
  EXPECT_EQ(doc["event_type"], "session.ended");
  EXPECT_EQ(doc["aggregate_type"], "reading_session");
  EXPECT_EQ(doc["aggregate_id"], std::string("session:") + kMd5A + ":2026-07-24T08:00:00Z");
  EXPECT_EQ(doc["payload"]["duration_seconds"], 600);
  EXPECT_EQ(doc["payload"]["started_at"], "2026-07-24T08:00:00Z");
  EXPECT_EQ(doc["payload"]["ended_at"], "2026-07-24T08:10:00Z");
  EXPECT_EQ(doc["payload"]["pages_read"], 12);
  EXPECT_EQ(doc["time_precision"], "anchored");

  JsonWriter w(nullptr, 0);
  s.ended.epochSec = s.started.epochSec + 59;
  EXPECT_FALSE(events::writeSession(w, Device::book(), s));
  s.ended.epochSec = s.started.epochSec + 60;
  EXPECT_TRUE(events::writeSession(w, Device::book(), s));
  s.ended.epochSec = s.started.epochSec + 14400;
  EXPECT_TRUE(events::writeSession(w, Device::book(), s));
  s.ended.epochSec = s.started.epochSec + 14401;
  EXPECT_FALSE(events::writeSession(w, Device::book(), s));
  s.ended.epochSec = s.started.epochSec + 600;
  s.started.precision = TimePrecision::Unknown;
  EXPECT_FALSE(events::writeSession(w, Device::book(), s));
}

namespace {
class Source : public IByteSource {
 public:
  explicit Source(std::string text) : text_(std::move(text)) {}
  int32_t read(uint8_t* dst, size_t len) override {
    if (pos_ >= text_.size()) return 0;
    const size_t n = std::min(len, text_.size() - pos_);
    memcpy(dst, text_.data() + pos_, n);
    pos_ += n;
    return static_cast<int32_t>(n);
  }
  bool seek(uint32_t pos) override {
    pos_ = pos;
    return true;
  }

 private:
  std::string text_;
  size_t pos_ = 0;
};
}  // namespace

TEST(RemoteEvents, ParsesTheServerShape) {
  Source source(remoteProgress(45, kOtherId, kMd5A, 60.5, "/body/p[60].0",
                               std::string("{\"suggestion\":{\"id\":\"") + "40000000-0000-4000-8000-000000000001" + "\"},\"local\":null}"));
  JsonScanner scan(source);
  auto e = std::make_unique<RemoteEvent>();
  ASSERT_TRUE(parseRemoteEvent(scan, *e));
  EXPECT_TRUE(e->hasEventId);
  EXPECT_STREQ(e->eventId, "evt-45");
  EXPECT_TRUE(e->hasSeq);
  EXPECT_EQ(e->serverSeq, 45u);
  EXPECT_TRUE(e->isProgress());
  EXPECT_STREQ(e->bookKind, "koreader_partial_md5");
  EXPECT_STREQ(e->bookValue, kMd5A);
  EXPECT_STREQ(e->originId, kOtherId);
  EXPECT_STREQ(e->originName, "Kobo");
  EXPECT_EQ(e->precision, TimePrecision::Exact);
  EXPECT_EQ(e->pctMicro, 60500000u);
  EXPECT_EQ(e->page, 10);
  EXPECT_EQ(e->totalPages, 100);
  EXPECT_TRUE(e->xpointerUsable);
  EXPECT_STREQ(e->xpointer, "/body/p[60].0");
  EXPECT_STREQ(e->suggestionId, "40000000-0000-4000-8000-000000000001");
}

TEST(RemoteEvents, AnXpointerThatDoesNotFitIsNoXpointer) {
  const std::string longXp(kMaxXpointer + 50, 'x');
  Source source(remoteProgress(1, kOtherId, kMd5A, 1.0, longXp));
  JsonScanner scan(source);
  auto e = std::make_unique<RemoteEvent>();
  ASSERT_TRUE(parseRemoteEvent(scan, *e));
  EXPECT_FALSE(e->xpointerUsable);
  EXPECT_EQ(e->xpointer[0], '\0');
}

TEST(RemoteEvents, HostileFieldsNeverFailTheElement) {
  const char* cases[] = {
      "5",
      "\"str\"",
      "[1,2]",
      "null",
      "{}",
      R"({"event_id":5,"server_sequence":6,"origin_device":"x","book_identifier":[],"payload":"p","directive_metadata":1})",
      R"({"payload":{"percentage":"60","current_page":"a","total_pages":-1,"xpointer":12,"explicit_jump":"no"}})",
      R"({"payload":{"percentage":1000}})",
      R"({"directive_metadata":{"suggestion":{"id":"../../etc"}}})",
      R"({"time_precision":"made-up","occurred_at":123})",
  };
  for (const char* text : cases) {
    Source source(text);
    JsonScanner scan(source);
    auto e = std::make_unique<RemoteEvent>();
    EXPECT_TRUE(parseRemoteEvent(scan, *e)) << text;
    EXPECT_FALSE(scan.failed()) << text;
    EXPECT_FALSE(e->hasPct) << text;
    EXPECT_EQ(e->suggestionId[0], '\0') << text;
    EXPECT_EQ(e->precision, TimePrecision::Unknown) << text;
  }
}
