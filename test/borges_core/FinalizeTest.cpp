#include "TestSupport.h"

using namespace borges;
using namespace borges_test;

namespace {

struct FakeClip {
  ClipKey key;
  std::string text;
  std::string xpointer;
  int page = 3;
};

class FakeClips : public IClipSource {
 public:
  std::vector<FakeClip> clips;
  bool failCount = false;
  long failKeyAt = -1;
  long failDetailAt = -1;
  bool count(size_t& out) override {
    if (failCount) return false;
    out = clips.size();
    return true;
  }
  bool key(size_t index, ClipKey& out) override {
    if (static_cast<long>(index) == failKeyAt) return false;
    out = clips[index].key;
    return true;
  }
  bool detail(size_t index, ClipDetail& out) override {
    if (static_cast<long>(index) == failDetailAt) return false;
    out.text = clips[index].text.c_str();
    out.xpointer = clips[index].xpointer.empty() ? nullptr : clips[index].xpointer.c_str();
    out.page = clips[index].page;
    return true;
  }
};

FakeClip clip(uint32_t spine, uint32_t offset, const std::string& text, bool bookmark = false) {
  FakeClip c;
  c.key.spine = spine;
  c.key.textOffset = offset;
  c.key.textHash = fnv1a64(text.data(), text.size());
  c.key.bookmark = bookmark;
  c.text = text;
  c.xpointer = "/body/p[" + std::to_string(offset) + "].0";
  return c;
}

class FinalizeTest : public testing::Test {
 protected:
  void SetUp() override { d.boot(); }
  Device d;

  std::vector<std::string> eventTypes() {
    std::vector<std::string> types;
    d.engine->runManualSession(nullptr, nullptr, nullptr);
    for (const auto& e : d.server.recorded) types.push_back(e.type);
    return types;
  }
};

}  // namespace

TEST_F(FinalizeTest, ABookThatWasNotPagedProducesNoEvent) {
  FinalizeInput input = d.finalizeInput(30);
  input.paged = false;
  const FinalizeResult result = d.engine->finalizeBook(input);
  EXPECT_TRUE(result.ok);
  EXPECT_FALSE(result.progressQueued);
  EXPECT_EQ(d.outbox->depth(), 0u);
}

TEST_F(FinalizeTest, OccurredAtIsTheLastRealPageTurn) {
  const int64_t turn = kNow - 500;
  FinalizeInput input = d.finalizeInput(30, "/body/p[1].0", turn);
  input.hasLastTurnMonotonic = true;
  input.lastTurnMonotonicMs = 4242;
  ASSERT_TRUE(d.engine->finalizeBook(input).progressQueued);
  d.engine->runManualSession(nullptr, nullptr, nullptr);
  ASSERT_EQ(d.server.recorded.size(), 1u);
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, d.server.recorded[0].body));
  char expected[21];
  formatRfc3339(turn, expected);
  EXPECT_EQ(doc["occurred_at"], expected);
  EXPECT_EQ(doc["time_precision"], "exact");
  EXPECT_EQ(doc["monotonic_ms"], "4242");
  EXPECT_EQ(doc["payload"]["percentage"], 30);
  EXPECT_EQ(doc["aggregate_id"], std::string("koreader_partial_md5:") + kMd5A);
}

TEST_F(FinalizeTest, ASecondFinalizeOfTheSameReadingIsNotANewEvent) {
  ASSERT_TRUE(d.engine->finalizeBook(d.finalizeInput(30)).progressQueued);
  const FinalizeResult again = d.engine->finalizeBook(d.finalizeInput(30));
  EXPECT_TRUE(again.ok);
  EXPECT_FALSE(again.progressQueued);
  EXPECT_EQ(d.outbox->depth(), 1u);
}

TEST_F(FinalizeTest, ReplacedUntilAttemptedThenImmutable) {
  ASSERT_TRUE(d.engine->finalizeBook(d.finalizeInput(30, "/a", kNow)).progressQueued);
  ASSERT_TRUE(d.engine->finalizeBook(d.finalizeInput(40, "/b", kNow + 10)).progressQueued);
  EXPECT_EQ(d.outbox->depth(), 1u) << "one unattempted progress event per book";

  // Attempted: the server may have it. A lost answer leaves it in the queue.
  d.http.handler = [this](const RecordedRequest& r) {
    Reply reply = d.server.handle(r);
    if (r.path == "/api/sync/v2/exchange") reply.dropAfterBytes = 0;
    return reply;
  };
  const SessionReport failed = d.engine->runManualSession(nullptr, nullptr, nullptr);
  EXPECT_EQ(failed.outcome, SessionOutcome::Transport);
  ASSERT_EQ(d.outbox->depth(), 1u);

  ASSERT_TRUE(d.engine->finalizeBook(d.finalizeInput(50, "/c", kNow + 20)).progressQueued);
  EXPECT_EQ(d.outbox->depth(), 2u) << "the attempted one must stay as it is";
  ASSERT_TRUE(d.engine->finalizeBook(d.finalizeInput(60, "/d", kNow + 30)).progressQueued);
  EXPECT_EQ(d.outbox->depth(), 2u) << "the new unattempted one is replaced";

  d.http.handler = [this](const RecordedRequest& r) { return d.server.handle(r); };
  EXPECT_EQ(d.engine->runManualSession(nullptr, nullptr, nullptr).outcome, SessionOutcome::Ok);
  ASSERT_EQ(d.server.recorded.size(), 2u);
  JsonDocument first, second;
  ASSERT_FALSE(deserializeJson(first, d.server.recorded[0].body));
  ASSERT_FALSE(deserializeJson(second, d.server.recorded[1].body));
  EXPECT_EQ(first["payload"]["percentage"], 40);
  EXPECT_EQ(second["payload"]["percentage"], 60);
  EXPECT_EQ(d.server.violations, std::vector<std::string>{});
}

TEST_F(FinalizeTest, EachBookHasItsOwnUnattemptedEvent) {
  FinalizeInput a = d.finalizeInput(30);
  FinalizeInput b = d.finalizeInput(40);
  b.book = Device::bookRef(kMd5B);
  ASSERT_TRUE(d.engine->finalizeBook(a).progressQueued);
  ASSERT_TRUE(d.engine->finalizeBook(b).progressQueued);
  EXPECT_EQ(d.outbox->depth(), 2u);
}

TEST_F(FinalizeTest, NotPairedQueuesNothing) {
  d.store->forgetCredential();
  const FinalizeResult result = d.engine->finalizeBook(d.finalizeInput(30));
  EXPECT_TRUE(result.ok);
  EXPECT_EQ(d.outbox->depth(), 0u);
}

TEST_F(FinalizeTest, FullQueueKeepsTheProgressForTheNextFinalize) {
  for (int i = 0; i < Outbox::kMaxLiveEvents; ++i) ASSERT_EQ(d.appendRaw("e"), Outbox::AppendResult::Ok);
  const FinalizeResult result = d.engine->finalizeBook(d.finalizeInput(30));
  EXPECT_FALSE(result.ok);
  EXPECT_FALSE(result.progressQueued);
  // The ledger did not move, so it is not mistaken for already done.
  BookLedger ledger;
  ASSERT_TRUE(d.store->loadBook(Device::book(), ledger));
  EXPECT_FALSE(ledger.hasFinalized);
}

TEST_F(FinalizeTest, SessionIsEmittedOnlyWithinTheRules) {
  SessionSummary good;
  good.started = WallTime{kNow - 600, TimePrecision::Exact};
  good.ended = WallTime{kNow, TimePrecision::Exact};
  good.pagesRead = 5;
  FinalizeInput input = d.finalizeInput(30);
  input.session = &good;
  EXPECT_TRUE(d.engine->finalizeBook(input).sessionQueued);

  SessionSummary tooShort = good;
  tooShort.started.epochSec = kNow - 59;
  input.session = &tooShort;
  EXPECT_FALSE(d.engine->finalizeBook(input).sessionQueued);

  SessionSummary tooLong = good;
  tooLong.started.epochSec = kNow - 14401;
  input.session = &tooLong;
  EXPECT_FALSE(d.engine->finalizeBook(input).sessionQueued);

  SessionSummary untimed = good;
  untimed.started.precision = TimePrecision::Unknown;
  input.session = &untimed;
  EXPECT_FALSE(d.engine->finalizeBook(input).sessionQueued);
}

// --- Highlights --------------------------------------------------------------------

TEST_F(FinalizeTest, NewClippingsBecomeCreatedRemovedOnesDeleted) {
  FakeClips clips;
  clips.clips = {clip(1, 10, "uno"), clip(2, 20, "dos")};
  FinalizeInput input = d.finalizeInput(30);
  input.paged = false;
  input.clips = &clips;
  FinalizeResult result = d.engine->finalizeBook(input);
  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.annotationsCreated, 2);
  EXPECT_EQ(d.outbox->depth(), 2u);

  // Nothing changed: nothing happens.
  result = d.engine->finalizeBook(input);
  EXPECT_EQ(result.annotationsCreated, 0);
  EXPECT_EQ(result.annotationsDeleted, 0);
  EXPECT_EQ(d.outbox->depth(), 2u);

  // One goes away.
  clips.clips.erase(clips.clips.begin());
  result = d.engine->finalizeBook(input);
  EXPECT_EQ(result.annotationsDeleted, 1);
  EXPECT_EQ(result.annotationsCreated, 0);
  EXPECT_EQ(d.outbox->depth(), 3u);

  EXPECT_EQ(eventTypes(), (std::vector<std::string>{"annotation.created", "annotation.created", "annotation.deleted"}));

  // The deleted one carries the sync_id its creation had.
  JsonDocument created, deleted;
  ASSERT_FALSE(deserializeJson(created, d.server.recorded[0].body));
  ASSERT_FALSE(deserializeJson(deleted, d.server.recorded[2].body));
  EXPECT_EQ(created["payload"]["sync_id"].as<std::string>(), deleted["payload"]["sync_id"].as<std::string>());
  EXPECT_EQ(deleted["aggregate_id"].as<std::string>(), deleted["payload"]["sync_id"].as<std::string>());
  EXPECT_EQ(created["aggregate_type"], "annotation");
  EXPECT_EQ(created["payload"]["color"], "gray");
  EXPECT_EQ(deleted["payload"]["book"]["title"], "Ficciones");
}

TEST_F(FinalizeTest, BookmarksUseBookmarkEvents) {
  FakeClips clips;
  clips.clips = {clip(1, 10, "marca", true)};
  FinalizeInput input = d.finalizeInput(30);
  input.paged = false;
  input.clips = &clips;
  d.engine->finalizeBook(input);
  clips.clips.clear();
  d.engine->finalizeBook(input);
  EXPECT_EQ(eventTypes(), (std::vector<std::string>{"bookmark.upserted", "bookmark.deleted"}));
}

TEST_F(FinalizeTest, UnreadableClippingsNeverMeanDeleted) {
  FakeClips clips;
  clips.clips = {clip(1, 10, "uno"), clip(2, 20, "dos")};
  FinalizeInput input = d.finalizeInput(30);
  input.paged = false;
  input.clips = &clips;
  ASSERT_EQ(d.engine->finalizeBook(input).annotationsCreated, 2);

  clips.failCount = true;
  FinalizeResult result = d.engine->finalizeBook(input);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.annotationsDeleted, 0);
  clips.failCount = false;

  clips.failKeyAt = 1;
  result = d.engine->finalizeBook(input);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.annotationsDeleted, 0);
  clips.failKeyAt = -1;

  EXPECT_EQ(d.outbox->depth(), 2u);
}

TEST_F(FinalizeTest, ClipThatCanNeverBeValidIsRecordedAndNotRetriedForever) {
  FakeClips clips;
  FakeClip bad = clip(1, 10, "");
  bad.text = "   ";
  clips.clips = {bad, clip(2, 20, "dos")};
  FinalizeInput input = d.finalizeInput(30);
  input.paged = false;
  input.clips = &clips;
  FinalizeResult result = d.engine->finalizeBook(input);
  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.annotationsCreated, 1);
  result = d.engine->finalizeBook(input);
  EXPECT_EQ(result.annotationsCreated, 0);
  EXPECT_EQ(d.outbox->depth(), 1u);
}

TEST_F(FinalizeTest, ALargeBatchOfNewClippingsGoesAcrossSeveralFinalizes) {
  d.config.maxAnnotationEvents = 3;
  d.boot();
  FakeClips clips;
  for (uint32_t i = 0; i < 5; ++i) clips.clips.push_back(clip(1, i, "t" + std::to_string(i)));
  FinalizeInput input = d.finalizeInput(30);
  input.paged = false;
  input.clips = &clips;
  FinalizeResult result = d.engine->finalizeBook(input);
  EXPECT_EQ(result.annotationsCreated, 3);
  EXPECT_TRUE(result.annotationsPending);
  result = d.engine->finalizeBook(input);
  EXPECT_EQ(result.annotationsCreated, 2);
  EXPECT_FALSE(result.annotationsPending);
  EXPECT_EQ(d.outbox->depth(), 5u);
}

TEST_F(FinalizeTest, PendingCreationsHoldBackDeletions) {
  // A partial view of what is new must not decide what is gone.
  d.config.maxAnnotationEvents = 1;
  d.boot();
  FakeClips clips;
  clips.clips = {clip(1, 1, "old")};
  FinalizeInput input = d.finalizeInput(30);
  input.paged = false;
  input.clips = &clips;
  ASSERT_EQ(d.engine->finalizeBook(input).annotationsCreated, 1);
  clips.clips = {clip(1, 2, "new-a"), clip(1, 3, "new-b")};
  FinalizeResult result = d.engine->finalizeBook(input);
  EXPECT_EQ(result.annotationsCreated, 1);
  EXPECT_EQ(result.annotationsDeleted, 0);
  EXPECT_TRUE(result.annotationsPending);
}

TEST_F(FinalizeTest, CrashBetweenQueueingAndLedgerDoesNotDuplicateTheAnnotation) {
  FakeClips clips;
  clips.clips = {clip(1, 10, "uno")};
  FinalizeInput input = d.finalizeInput(30);
  input.paged = false;
  input.clips = &clips;
  // The ledger write is what fails: simulates dying right after the event went durable.
  d.fs.failWritesTo(std::string("/.crosspoint/borges/ann/") + kMd5A + ".a");
  EXPECT_FALSE(d.engine->finalizeBook(input).ok);
  ASSERT_EQ(d.outbox->depth(), 1u);
  d.fs.failWritesTo("");
  // It looks new again, but gets the same sync_id, so the server sees one annotation.
  EXPECT_EQ(d.engine->finalizeBook(input).annotationsCreated, 1);
  d.engine->runManualSession(nullptr, nullptr, nullptr);
  ASSERT_EQ(d.server.recorded.size(), 2u);
  JsonDocument a, b;
  ASSERT_FALSE(deserializeJson(a, d.server.recorded[0].body));
  ASSERT_FALSE(deserializeJson(b, d.server.recorded[1].body));
  EXPECT_EQ(a["aggregate_id"].as<std::string>(), b["aggregate_id"].as<std::string>());
  EXPECT_NE(a["client_event_id"].as<std::string>(), b["client_event_id"].as<std::string>());
}

TEST_F(FinalizeTest, ClockUnknownEventsAreHonest) {
  FinalizeInput input = d.finalizeInput(30);
  input.lastTurn.precision = TimePrecision::Unknown;
  ASSERT_TRUE(d.engine->finalizeBook(input).progressQueued);
  d.clock.wall.precision = TimePrecision::Exact;
  d.engine->runManualSession(nullptr, nullptr, nullptr);
  ASSERT_EQ(d.server.recorded.size(), 1u);
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, d.server.recorded[0].body));
  EXPECT_TRUE(doc["occurred_at"].isNull());
  EXPECT_EQ(doc["time_precision"], "unknown");
}
