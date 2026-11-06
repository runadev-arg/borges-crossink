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
  int details = 0;
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
    ++details;
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

// Found by the contract tests: ok stayed true, and a caller trusting it dropped the session.
TEST_F(FinalizeTest, FullQueueReportsASessionItCouldNotKeep) {
  for (int i = 0; i < Outbox::kMaxLiveEvents; ++i) ASSERT_EQ(d.appendRaw("e"), Outbox::AppendResult::Ok);
  SessionSummary good;
  good.started = WallTime{kNow - 600, TimePrecision::Exact};
  good.ended = WallTime{kNow, TimePrecision::Exact};
  FinalizeInput input = d.finalizeInput(30);
  input.paged = false;
  input.session = &good;
  const FinalizeResult result = d.engine->finalizeBook(input);
  EXPECT_FALSE(result.sessionQueued);
  EXPECT_FALSE(result.ok);

  // A session the rules refuse is not a failure: there is nothing to try again.
  SessionSummary tooShort = good;
  tooShort.started.epochSec = kNow - 59;
  input.session = &tooShort;
  EXPECT_TRUE(d.engine->finalizeBook(input).ok);
}

TEST_F(FinalizeTest, PageTimeGoesOutAtFinalizeAndLeavesTheBuffer) {
  PageStatBuffer rows;
  for (int i = 0; i < 70; ++i) rows.onLeave(10 + i, kNow - 7000 + i * 100, kNow - 7000 + i * 100 + 90);
  FinalizeInput input = d.finalizeInput(30);
  input.pageStats = &rows;
  const FinalizeResult result = d.engine->finalizeBook(input);
  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.pageStatEvents, 3) << "32 + 32 + 6";
  EXPECT_EQ(rows.count(), 0);
  ASSERT_EQ(d.engine->runManualSession(nullptr, nullptr, nullptr).outcome, SessionOutcome::Ok);
  size_t sent = 0;
  std::set<std::string> ids;
  for (const auto& e : d.server.recorded) {
    if (e.type != "page_stat.recorded") continue;
    JsonDocument doc;
    ASSERT_FALSE(deserializeJson(doc, e.body));
    sent += doc["payload"]["rows"].as<JsonArray>().size();
    ids.insert(doc["aggregate_id"].as<std::string>());
    EXPECT_EQ(doc["payload"]["books"].as<JsonArray>().size(), 1u) << "every chunk stands alone";
  }
  EXPECT_EQ(sent, 70u);
  EXPECT_EQ(ids.size(), 3u);
  EXPECT_EQ(d.server.violations, std::vector<std::string>{});
}

TEST_F(FinalizeTest, PageTimeWithAClockNobodyVouchesForIsNotReported) {
  PageStatBuffer rows;
  rows.onLeave(1, kNow - 100, kNow);
  FinalizeInput input = d.finalizeInput(30);
  input.lastTurn.precision = TimePrecision::Approximate;
  input.pageStats = &rows;
  EXPECT_EQ(d.engine->finalizeBook(input).pageStatEvents, 0);
  EXPECT_EQ(rows.count(), 0);
}

TEST_F(FinalizeTest, PageTimeNeverTakesTheLastPlacesInTheQueue) {
  for (int i = 0; i < Outbox::kMaxLiveEvents - 32; ++i) ASSERT_EQ(d.appendRaw("e"), Outbox::AppendResult::Ok);
  PageStatBuffer rows;
  rows.onLeave(1, kNow - 100, kNow);
  FinalizeInput input = d.finalizeInput(30);
  input.paged = false;
  input.pageStats = &rows;
  const FinalizeResult result = d.engine->finalizeBook(input);
  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.pageStatEvents, 0);
  EXPECT_EQ(d.outbox->depth(), static_cast<size_t>(Outbox::kMaxLiveEvents - 32));
}

TEST_F(FinalizeTest, PageTimeThatCouldNotBeQueuedStaysForTheNextFinalize) {
  PageStatBuffer rows;
  rows.onLeave(1, kNow - 100, kNow);
  d.fs.failWritesTo(std::string(kOutboxDir) + "/outbox.1.log");
  FinalizeInput input = d.finalizeInput(30);
  input.paged = false;
  input.pageStats = &rows;
  EXPECT_FALSE(d.engine->finalizeBook(input).ok);
  EXPECT_EQ(rows.count(), 1);
}

TEST_F(FinalizeTest, ALongReadingIsSplitIntoSessionsTheServerAccepts) {
  SessionSummary s;
  s.started = WallTime{kNow - 9 * 3600, TimePrecision::Exact};
  s.ended = WallTime{kNow, TimePrecision::Exact};
  s.pagesRead = 300;
  s.startPage = 10;
  s.endPage = 310;
  FinalizeInput input = d.finalizeInput(30);
  input.session = &s;
  const FinalizeResult result = d.engine->finalizeBook(input);
  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.sessionQueued);
  ASSERT_EQ(d.engine->runManualSession(nullptr, nullptr, nullptr).outcome, SessionOutcome::Ok);

  std::vector<JsonDocument> sessions;
  for (const auto& e : d.server.recorded) {
    if (e.type != "session.ended") continue;
    sessions.emplace_back();
    ASSERT_FALSE(deserializeJson(sessions.back(), e.body));
  }
  ASSERT_EQ(sessions.size(), 3u) << "4 h + 4 h + 1 h";
  int64_t total = 0;
  for (auto& doc : sessions) {
    const int64_t seconds = doc["payload"]["duration_seconds"];
    EXPECT_GE(seconds, events::kMinSessionSec);
    EXPECT_LE(seconds, events::kMaxSessionSec);
    total += seconds;
  }
  EXPECT_EQ(total, 9 * 3600) << "no time lost at the seams";
  EXPECT_EQ(sessions[0]["payload"]["start_page"], 10);
  EXPECT_EQ(sessions[0]["payload"]["pages_read"], 0);
  EXPECT_EQ(sessions[2]["payload"]["pages_read"], 300) << "counted once";
  EXPECT_EQ(sessions[2]["payload"]["end_page"], 310);
  EXPECT_NE(sessions[0]["aggregate_id"].as<std::string>(), sessions[1]["aggregate_id"].as<std::string>());
}

TEST_F(FinalizeTest, EverySplitSessionsDurationIsExactlyItsOwnTimesAndItsIdIsItsOwn) {
  // The server clamps duration to ended_at - started_at, refuses under a minute after that, and
  // dedupes on device + aggregate_id. A piece has to survive all three as it is.
  SessionSummary s;
  s.started = WallTime{kNow - 2 * events::kMaxSessionSec - 90, TimePrecision::Exact};
  s.ended = WallTime{kNow, TimePrecision::Exact};
  s.pagesRead = 9;
  FinalizeInput input = d.finalizeInput(30);
  input.session = &s;
  ASSERT_TRUE(d.engine->finalizeBook(input).sessionQueued);
  ASSERT_EQ(d.engine->runManualSession(nullptr, nullptr, nullptr).outcome, SessionOutcome::Ok);
  std::set<std::string> ids;
  size_t pieces = 0;
  for (const auto& e : d.server.recorded) {
    if (e.type != "session.ended") continue;
    ++pieces;
    JsonDocument doc;
    ASSERT_FALSE(deserializeJson(doc, e.body));
    int64_t started = 0;
    int64_t ended = 0;
    ASSERT_TRUE(parseRfc3339(doc["payload"]["started_at"].as<const char*>(), started));
    ASSERT_TRUE(parseRfc3339(doc["payload"]["ended_at"].as<const char*>(), ended));
    EXPECT_EQ(doc["payload"]["duration_seconds"].as<int64_t>(), ended - started);
    EXPECT_GE(ended - started, events::kMinSessionSec);
    ids.insert(doc["aggregate_id"].as<std::string>());
  }
  EXPECT_EQ(pieces, 3u);
  EXPECT_EQ(ids.size(), pieces);
}

TEST_F(FinalizeTest, ALastPieceUnderAMinuteIsNotASession) {
  SessionSummary s;
  s.started = WallTime{kNow - events::kMaxSessionSec - 30, TimePrecision::Exact};
  s.ended = WallTime{kNow, TimePrecision::Exact};
  s.pagesRead = 5;
  FinalizeInput input = d.finalizeInput(30);
  input.session = &s;
  ASSERT_TRUE(d.engine->finalizeBook(input).ok);
  size_t sessions = 0;
  for (size_t i = 0; i < d.outbox->depth(); ++i) {
    if (d.outbox->entryAt(i)->kind == static_cast<uint8_t>(EventKind::Session)) ++sessions;
  }
  EXPECT_EQ(sessions, 1u);
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

  SessionSummary untimed = good;
  untimed.started.precision = TimePrecision::Unknown;
  input.session = &untimed;
  EXPECT_FALSE(d.engine->finalizeBook(input).sessionQueued);
}

TEST_F(FinalizeTest, ATooShortVisitIsNotASessionThatFailed) {
  // A failing clipping says nothing about it: "not queued" and "could not queue" are different, and
  // only the second is worth keeping for later.
  SessionSummary visit;
  visit.started = WallTime{kNow - 30, TimePrecision::Exact};
  visit.ended = WallTime{kNow, TimePrecision::Exact};
  FakeClips clips;
  clips.clips.push_back(clip(1, 1, "uno"));
  clips.failDetailAt = 0;
  FinalizeInput input = d.finalizeInput(30);
  input.session = &visit;
  input.clips = &clips;
  const FinalizeResult result = d.engine->finalizeBook(input);
  EXPECT_FALSE(result.ok);
  EXPECT_FALSE(result.sessionQueued);
  EXPECT_FALSE(result.sessionFailed);
}

TEST_F(FinalizeTest, ASessionThatCouldNotBeQueuedSaysSo) {
  SessionSummary good;
  good.started = WallTime{kNow - 600, TimePrecision::Exact};
  good.ended = WallTime{kNow, TimePrecision::Exact};
  d.fs.failWritesTo(std::string(kOutboxDir) + "/outbox.1.log");
  FinalizeInput input = d.finalizeInput(30);
  input.paged = false;
  input.session = &good;
  const FinalizeResult result = d.engine->finalizeBook(input);
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.sessionFailed);
}

// --- Highlights --------------------------------------------------------------------

class AccountChangeClips : public FinalizeTest {
 protected:
  void SetUp() override {
    FinalizeTest::SetUp();
    copyText(d.store->creds().accountId, sizeof(d.store->creds().accountId), "account-a");
  }
  FakeClips clips;
  size_t finalizeAndSend() {
    FinalizeInput input = d.finalizeInput(10);
    input.paged = false;
    input.clips = &clips;
    EXPECT_TRUE(d.engine->finalizeBook(input).ok);
    d.engine->runManualSession(nullptr, nullptr, nullptr);
    return d.server.recorded.size();
  }
};

TEST_F(AccountChangeClips, WhatIsOnTheCardDoesNotGoUpUnderTheNewAccount) {
  clips.clips.push_back(clip(1, 1, "de A"));
  clips.clips.push_back(clip(1, 2, "marca de A", true));
  ASSERT_EQ(finalizeAndSend(), 2u);
  d.server.recorded.clear();
  d.server.byId.clear();

  ASSERT_TRUE(d.store->adoptAccount("account-b", nullptr, *d.outbox));
  EXPECT_EQ(finalizeAndSend(), 0u);
  EXPECT_EQ(clips.details, 2) << "and nothing was even resolved for it";

  // Made from here on, it is this account's.
  clips.clips.push_back(clip(1, 3, "de B"));
  EXPECT_EQ(finalizeAndSend(), 1u);
  EXPECT_TRUE(d.server.hasText("de B"));
}

TEST_F(AccountChangeClips, ABookNeverFinalizedBeforeHasNoLedgerAndIsSeededAllTheSame) {
  clips.clips.push_back(clip(1, 1, "de A, nunca enviado"));
  ASSERT_TRUE(d.store->adoptAccount("account-b", nullptr, *d.outbox));
  EXPECT_EQ(finalizeAndSend(), 0u);
}

TEST_F(AccountChangeClips, GoingBackPublishesNothingAgainAndBringsNothingOfTheOther) {
  clips.clips.push_back(clip(1, 1, "de A"));
  ASSERT_EQ(finalizeAndSend(), 1u);
  ASSERT_TRUE(d.store->adoptAccount("account-b", nullptr, *d.outbox));
  ASSERT_EQ(finalizeAndSend(), 1u);
  clips.clips.push_back(clip(1, 2, "de B"));
  ASSERT_EQ(finalizeAndSend(), 2u);
  d.server.recorded.clear();
  d.server.byId.clear();

  // A's highlight may have been deleted on the web meanwhile: sending it again would bring it back
  // as a conflict. B's is not A's business.
  ASSERT_TRUE(d.store->adoptAccount("account-a", nullptr, *d.outbox));
  EXPECT_EQ(finalizeAndSend(), 0u);
}

TEST_F(AccountChangeClips, DeletingWhatWasNeverSentTellsTheServerNothing) {
  clips.clips.push_back(clip(1, 1, "de A"));
  ASSERT_TRUE(d.store->adoptAccount("account-b", nullptr, *d.outbox));
  ASSERT_EQ(finalizeAndSend(), 0u);
  clips.clips.clear();
  EXPECT_EQ(finalizeAndSend(), 0u);
  AnnotationRecord* records = nullptr;
  uint16_t count = 9;
  ASSERT_TRUE(d.store->loadAnnotations(Device::book(), &records, &count));
  EXPECT_EQ(count, 0);
  delete[] records;
}

TEST_F(AccountChangeClips, SeedingAtTheOpenIsWhatLetsTheFirstSessionPublish) {
  clips.clips.push_back(clip(1, 1, "de A"));
  ASSERT_TRUE(d.store->adoptAccount("account-b", nullptr, *d.outbox));
  ASSERT_TRUE(d.engine->seedClipsIfForeign(Device::book(), clips));
  clips.clips.push_back(clip(1, 2, "hecho en esta lectura"));
  EXPECT_EQ(finalizeAndSend(), 1u);
  EXPECT_TRUE(d.server.hasText("hecho en esta lectura"));
  // And it only ever happens once per account.
  clips.clips.push_back(clip(1, 3, "otro"));
  ASSERT_TRUE(d.engine->seedClipsIfForeign(Device::book(), clips));
  EXPECT_EQ(finalizeAndSend(), 2u);
}

TEST_F(AccountChangeClips, AnEmptyBookCountsAsSeeded) {
  ASSERT_TRUE(d.store->adoptAccount("account-b", nullptr, *d.outbox));
  ASSERT_TRUE(d.engine->seedClipsIfForeign(Device::book(), clips));
  clips.clips.push_back(clip(1, 1, "el primero"));
  EXPECT_EQ(finalizeAndSend(), 1u);
}

TEST_F(FinalizeTest, TheFirstAccountOfACardPublishesWhatIsThere) {
  // Pairing for the first time is not taking over from anybody: these are the person's own.
  FakeClips clips;
  clips.clips.push_back(clip(1, 1, "de antes de vincular"));
  ASSERT_TRUE(d.store->adoptAccount("account-a", "ana", *d.outbox));
  EXPECT_LT(d.store->state().accountEpoch, kFirstMovedEpoch);
  FinalizeInput input = d.finalizeInput(10);
  input.paged = false;
  input.clips = &clips;
  EXPECT_EQ(d.engine->finalizeBook(input).annotationsCreated, 1);
}

TEST_F(FinalizeTest, TheSameKeyTwiceInTheLedgerIsNotOneOfThemDeleted) {
  FakeClip mark = clip(1, 1, "marca", true);
  KnownAnnotation known;
  known.key = events::clipLedgerKey(Device::book(), mark.key);
  events::deriveSyncId(Device::book(), mark.key, known.syncId);
  AnnotationRecord twice[2] = {};
  for (AnnotationRecord& r : twice) {
    r.key = known.key;
    memcpy(r.syncId, known.syncId, 16);
    r.bookmark = kAnnBookmark;
  }
  ASSERT_TRUE(d.store->saveAnnotations(Device::book(), twice, 2));
  FakeClips clips;
  clips.clips.push_back(mark);
  FinalizeInput input = d.finalizeInput(10);
  input.paged = false;
  input.clips = &clips;
  const FinalizeResult result = d.engine->finalizeBook(input);
  EXPECT_EQ(result.annotationsDeleted, 0) << "it is right there";

  // Gone for real: one delete, not two.
  clips.clips.clear();
  EXPECT_EQ(d.engine->finalizeBook(input).annotationsDeleted, 1);
}

TEST_F(FinalizeTest, AClipThatNothingLocatesYetIsNotMarkedAsSent) {
  // The book closed before a page settled: no page counts, and no xpointer for this one.
  FakeClips clips;
  FakeClip unplaced = clip(1, 1, "sin lugar");
  unplaced.xpointer.clear();
  unplaced.page = -1;
  clips.clips.push_back(unplaced);
  FinalizeInput input = d.finalizeInput(10);
  input.paged = false;
  input.clips = &clips;
  input.book.totalPages = -1;
  FinalizeResult result = d.engine->finalizeBook(input);
  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.annotationsPending);
  EXPECT_EQ(result.annotationsCreated, 0);
  EXPECT_EQ(d.outbox->depth(), 0u);

  // The next exit knows.
  clips.clips[0].page = 12;
  result = d.engine->finalizeBook(input);
  EXPECT_EQ(result.annotationsCreated, 1);
}

TEST_F(FinalizeTest, AHighlightOfNothingIsRecordedAndStopsComingBack) {
  FakeClips clips;
  clips.clips.push_back(clip(1, 1, "   "));
  FinalizeInput input = d.finalizeInput(10);
  input.paged = false;
  input.clips = &clips;
  EXPECT_EQ(d.engine->finalizeBook(input).annotationsCreated, 0);
  const int before = clips.details;
  EXPECT_FALSE(d.engine->finalizeBook(input).annotationsPending);
  EXPECT_EQ(clips.details, before) << "already known";
}

TEST_F(FinalizeTest, MoreClippingsThanTheLedgerHoldsNeverReadAsDeleted) {
  // Past the limit the ledger cannot know them, and their absence from the read proves nothing.
  FakeClips clips;
  for (uint32_t i = 0; i < Store::kMaxAnnotations; ++i) clips.clips.push_back(clip(1, i, "t" + std::to_string(i)));
  FinalizeResult result;
  int exits = 0;
  do {
    FinalizeInput input = d.finalizeInput(10);
    input.paged = false;
    input.clips = &clips;
    result = d.engine->finalizeBook(input);
    ASSERT_TRUE(result.ok);
    d.outbox->reset();
  } while (result.annotationsPending && ++exits < 40);
  EXPECT_FALSE(result.annotationsPending);

  // A full ledger stays readable: no republishing at every exit.
  FinalizeInput same = d.finalizeInput(10);
  same.paged = false;
  same.clips = &clips;
  result = d.engine->finalizeBook(same);
  EXPECT_EQ(result.annotationsCreated, 0u);

  // One more up front pushes the last known clip past the cap.
  clips.clips.insert(clips.clips.begin(), clip(0, 0, "nueva"));
  FinalizeInput input = d.finalizeInput(10);
  input.paged = false;
  input.clips = &clips;
  result = d.engine->finalizeBook(input);
  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.annotationsPending);
  EXPECT_EQ(result.annotationsDeleted, 0u);
}

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
  d.fs.failWritesTo(std::string("/.crosspoint/borges/ann/ab/") + (kMd5A + 2) + ".a");
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
