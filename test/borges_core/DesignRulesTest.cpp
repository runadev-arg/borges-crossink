// One regression test per rule of docs/borges-sync.md section 4, named after it. Rules
// 7, 8 and 13 live mostly in the device glue (TLS, DNS, Wi-Fi); their tests here cover
// the part that is core, and say so.

#include "TestSupport.h"

using namespace borges;
using namespace borges_test;

namespace {

class DesignRule : public testing::Test {
 protected:
  void SetUp() override { d.boot(); }
  Device d;
  SessionReport manual() { return d.engine->runManualSession(nullptr, nullptr, nullptr); }
};

}  // namespace

// 1. Spool, never buffer.
TEST_F(DesignRule, Rule01_SpoolNeverBuffer_ResponsesGoThroughTheSpoolFile) {
  d.http.script.push_back(jsonReply(200, "{\"protocol_version\":2,\"events\":[],\"cursor\":\"5\",\"has_more\":false}"));
  manual();
  EXPECT_TRUE(d.fs.has(kSpoolPath));
  EXPECT_EQ(std::string(kSpoolPath).rfind("/.crosspoint/borges/spool/", 0), 0u);
}

TEST_F(DesignRule, Rule01_SpoolNeverBuffer_NoResponseSizeCapToWedgeOn) {
  d.server.delivered = 1u << 30;  // These replies are scripted, past the model server.
  // Four megabytes of page: far past any heap on this device.
  std::string events;
  for (uint64_t seq = 1; seq <= 1500; ++seq) {
    if (!events.empty()) events += ",";
    events += remoteProgress(seq, kOtherId, kMd5B, 1.0) + std::string(2000, ' ');
  }
  d.http.chunk = 2048;
  d.http.script.push_back(jsonReply(200, "{\"protocol_version\":2,\"events\":[" + events + "],\"cursor\":\"1500\",\"has_more\":false}"));
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.store->state().cursor, 1500u);
}

TEST_F(DesignRule, Rule01_SpoolNeverBuffer_OversizedEventSkippedWithCursorAdvancing) {
  d.server.delivered = 1u << 30;  // These replies are scripted, past the model server.
  std::string poison = remoteProgress(11, kOtherId, kMd5B, 99);
  poison.insert(poison.size() - 1, ",\"padding\":\"" + std::string(d.config.eventParseBudget, 'p') + "\"");
  d.http.script.push_back(jsonReply(200, "{\"protocol_version\":2,\"events\":[" + poison + "],\"cursor\":\"11\",\"has_more\":false}"));
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::Ok);
  EXPECT_EQ(report.eventsSkipped, 1);
  EXPECT_EQ(report.eventsApplied, 0);
  EXPECT_EQ(d.store->state().cursor, 11u);
  EXPECT_EQ(d.engine->status().skippedEvents, 1u);
  EXPECT_EQ(d.store->state().lastSkippedSeq, 11u);
}

// 2. One TLS connection per session, keep-alive, reconnect once on a dropped socket.
TEST_F(DesignRule, Rule02_OneConnectionPerSession_ManyRequestsOneConnection) {
  for (int i = 0; i < 45; ++i) ASSERT_EQ(d.appendRaw("e"), Outbox::AppendResult::Ok);
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_GT(d.http.log.size(), 4u);
  EXPECT_EQ(d.http.connections, 1);
  EXPECT_EQ(d.http.disconnects, 1);
}

TEST_F(DesignRule, Rule02_OneConnectionPerSession_ReconnectsExactlyOnce) {
  d.http.script.push_back(dropped());
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.http.connections, 2);

  d.http.connections = 0;
  d.http.script.push_back(dropped());
  d.http.script.push_back(dropped());
  EXPECT_EQ(manual().outcome, SessionOutcome::Transport);
  EXPECT_EQ(d.http.connections, 2);
}

// 3. Request bodies are bounded.
TEST_F(DesignRule, Rule03_RequestBodiesAreBounded_ByBytesAndEvents) {
  for (int i = 0; i < 60; ++i) ASSERT_EQ(d.appendRaw("e" + std::to_string(i), nullptr, kMd5A, 500), Outbox::AppendResult::Ok);
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  size_t exchanges = 0;
  for (const auto& r : d.http.log) {
    if (r.path != "/api/sync/v2/exchange") continue;
    ++exchanges;
    EXPECT_LE(r.body.size(), 8u * 1024 + 256);
    JsonDocument doc;
    ASSERT_FALSE(deserializeJson(doc, r.body));
    EXPECT_GE(doc["events"].size(), 1u);
    EXPECT_LE(doc["events"].size(), 20u);
  }
  EXPECT_GT(exchanges, 2u);
  EXPECT_EQ(d.outbox->depth(), 0u);
}

TEST_F(DesignRule, Rule03_RequestBodiesAreBounded_AtMostTwentyEvents) {
  for (int i = 0; i < 45; ++i) ASSERT_EQ(d.appendRaw("e"), Outbox::AppendResult::Ok);
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  for (const auto& r : d.http.log) {
    if (r.path != "/api/sync/v2/exchange") continue;
    JsonDocument doc;
    ASSERT_FALSE(deserializeJson(doc, r.body));
    EXPECT_LE(doc["events"].size(), 20u);
  }
}

TEST_F(DesignRule, Rule03_RequestBodiesAreBounded_OversizedEventSentAlone) {
  ASSERT_EQ(d.appendRaw("small-1"), Outbox::AppendResult::Ok);
  ASSERT_EQ(d.appendRaw("huge", nullptr, kMd5A, 20000), Outbox::AppendResult::Ok);
  ASSERT_EQ(d.appendRaw("small-2"), Outbox::AppendResult::Ok);
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  bool hugeSeen = false;
  for (const auto& r : d.http.log) {
    if (r.path != "/api/sync/v2/exchange" || r.body.find("huge") == std::string::npos) continue;
    hugeSeen = true;
    JsonDocument doc;
    ASSERT_FALSE(deserializeJson(doc, r.body));
    EXPECT_EQ(doc["events"].size(), 1u);
    EXPECT_LT(r.body.size(), 256u * 1024);
  }
  EXPECT_TRUE(hugeSeen);
  EXPECT_EQ(d.outbox->depth(), 0u);
}

// 4. Outbox cannot be poisoned.
TEST_F(DesignRule, Rule04_OutboxCannotBePoisoned_CorruptRecordQuarantinedAndSkipped) {
  std::string a, b, c;
  ASSERT_EQ(d.appendRaw("a", &a), Outbox::AppendResult::Ok);
  ASSERT_EQ(d.appendRaw("b", &b), Outbox::AppendResult::Ok);
  ASSERT_EQ(d.appendRaw("c", &c), Outbox::AppendResult::Ok);
  const std::string log = std::string(kOutboxDir) + "/outbox.1.log";
  const uint32_t middle = d.outbox->entryAt(1)->offset;
  d.fs.corrupt(log, middle + 60);

  d.boot();
  EXPECT_EQ(d.outbox->depth(), 2u);
  EXPECT_EQ(d.outbox->quarantinedRecords(), 1u);
  EXPECT_TRUE(d.fs.has(std::string(kOutboxDir) + "/outbox.bad"));

  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.outbox->depth(), 0u);
  EXPECT_EQ(d.server.byId.count(a), 1u);
  EXPECT_EQ(d.server.byId.count(b), 0u);
  EXPECT_EQ(d.server.byId.count(c), 1u);
}

TEST_F(DesignRule, Rule04_OutboxCannotBePoisoned_DepthCountsExactlyWhatNextBatchReturns) {
  std::vector<std::string> ids(12);
  for (auto& id : ids) ASSERT_EQ(d.appendRaw("e", &id), Outbox::AppendResult::Ok);
  const std::string log = std::string(kOutboxDir) + "/outbox.1.log";
  // Damage in every shape: a header, a body, and a length field.
  d.fs.corrupt(log, d.outbox->entryAt(2)->offset);
  d.fs.corrupt(log, d.outbox->entryAt(5)->offset + 80);
  d.fs.corrupt(log, d.outbox->entryAt(9)->offset + 5);
  d.boot();
  EXPECT_EQ(d.outbox->depth(), 9u);

  // Everything nextBatch can return, one batch at a time.
  size_t returned = 0;
  for (int guard = 0; guard < 100 && d.outbox->depth() > 0; ++guard) {
    const size_t before = d.outbox->depth();
    Batch batch;
    d.outbox->selectBatch(20, 1024 * 1024, nullptr, nullptr, 0, batch);
    ASSERT_EQ(batch.count, std::min<size_t>(before, 20));
    uint32_t offsets[20];
    size_t n = 0;
    for (uint8_t i = 0; i < batch.count; ++i) {
      std::string buffer(4096, '\0');
      JsonWriter w(buffer.data(), buffer.size());
      w.arrayBegin();
      ASSERT_EQ(d.outbox->writeEnvelope(batch.items[i], w), Outbox::EnvelopeResult::Ok);
      offsets[n++] = batch.items[i].offset;
    }
    returned += n;
    ASSERT_TRUE(d.outbox->ack(offsets, n));
  }
  EXPECT_EQ(returned, 9u);
}

TEST_F(DesignRule, Rule04_OutboxCannotBePoisoned_RotAfterOpenIsCaughtAtSendTime) {
  std::string a, b;
  ASSERT_EQ(d.appendRaw("a", &a), Outbox::AppendResult::Ok);
  ASSERT_EQ(d.appendRaw("b", &b), Outbox::AppendResult::Ok);
  EXPECT_EQ(d.outbox->depth(), 2u);
  d.fs.corrupt(std::string(kOutboxDir) + "/outbox.1.log", d.outbox->entryAt(0)->offset + 60);
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::Ok);
  EXPECT_EQ(report.eventsQuarantined, 1);
  EXPECT_EQ(report.depth, 0u);
  EXPECT_EQ(d.server.byId.count(a), 0u);
  EXPECT_EQ(d.server.byId.count(b), 1u);
}

TEST_F(DesignRule, Rule04_OutboxCannotBePoisoned_APassingReadErrorDiscardsNothing) {
  std::string a;
  ASSERT_EQ(d.appendRaw("a", &a), Outbox::AppendResult::Ok);
  d.fs.failReadsAfter(std::string(kOutboxDir) + "/outbox.1.log", 0);
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::LocalError);
  EXPECT_STREQ(d.store->state().diag.code, "outbox_read");
  EXPECT_EQ(report.eventsQuarantined, 0);
  EXPECT_EQ(d.outbox->depth(), 1u);
  EXPECT_FALSE(d.fs.has(std::string(kOutboxDir) + "/outbox.bad"));
  d.fs.failReadsAfter("", 0);
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.server.byId.count(a), 1u);
}

// 5. No per-page disk churn.
TEST_F(DesignRule, Rule05_NoPerPageDiskChurn_NothingWrittenWithoutFinalize) {
  // The core has no page-turn entry point at all: a turn cannot reach the card through
  // it. What it must do is stay silent on finalizing a book that was not paged.
  d.fs.resetWriteCounts();
  const uint64_t opsBefore = d.fs.ops();
  FinalizeInput input = d.finalizeInput(30);
  input.paged = false;
  EXPECT_TRUE(d.engine->finalizeBook(input).ok);
  EXPECT_EQ(d.fs.ops(), opsBefore);
}

TEST_F(DesignRule, Rule05_NoPerPageDiskChurn_OccurredAtIsLastTurnAndOneEventPerBook) {
  FinalizeInput first = d.finalizeInput(30, "/a", kNow - 100);
  FinalizeInput second = d.finalizeInput(40, "/b", kNow - 50);
  ASSERT_TRUE(d.engine->finalizeBook(first).progressQueued);
  ASSERT_TRUE(d.engine->finalizeBook(second).progressQueued);
  EXPECT_EQ(d.outbox->depth(), 1u);
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  ASSERT_EQ(d.server.recorded.size(), 1u);
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, d.server.recorded[0].body));
  char expected[21];
  formatRfc3339(kNow - 50, expected);
  EXPECT_EQ(doc["occurred_at"], expected);
  EXPECT_EQ(doc["payload"]["percentage"], 40);
}

// 6. Honest clock.
TEST_F(DesignRule, Rule06_HonestClock_LabelsComeFromTheClockNeverInvented) {
  FinalizeInput input = d.finalizeInput(30);
  input.lastTurn.precision = TimePrecision::Anchored;
  ASSERT_TRUE(d.engine->finalizeBook(input).progressQueued);
  manual();
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, d.server.recorded[0].body));
  EXPECT_EQ(doc["time_precision"], "anchored");
}

TEST_F(DesignRule, Rule06_HonestClock_ServerTimeFixesAWorseClockOnly) {
  d.clock.wall = WallTime{0, TimePrecision::Unknown};
  manual();
  EXPECT_EQ(d.clock.setCalls, 1);
  EXPECT_EQ(d.clock.wall.precision, TimePrecision::Exact);
  int64_t serverTime = 0;
  ASSERT_TRUE(parseRfc3339("2026-11-05T12:00:00.000Z", serverTime));
  EXPECT_EQ(d.clock.wall.epochSec, serverTime);
  EXPECT_EQ(d.store->state().anchorEpoch, serverTime);

  // Exact stays untouched, even with a different server_time.
  d.clock.wall = WallTime{kNow, TimePrecision::Exact};
  d.clock.setCalls = 0;
  manual();
  EXPECT_EQ(d.clock.setCalls, 0);
  EXPECT_EQ(d.clock.wall.epochSec, kNow);

  // Anchored is worse than a fresh server_time.
  d.clock.wall = WallTime{kNow, TimePrecision::Anchored};
  manual();
  EXPECT_EQ(d.clock.setCalls, 1);
}

TEST_F(DesignRule, Rule06_HonestClock_AnchorSurvivesDeepSleep) {
  manual();
  const int64_t anchor = d.store->state().anchorEpoch;
  EXPECT_GT(anchor, 0);
  d.crashAndBoot();
  EXPECT_EQ(d.store->state().anchorEpoch, anchor);
  EXPECT_EQ(clockpolicy::labelAfterBoot(false, anchor + 3600, d.store->state().anchorEpoch), TimePrecision::Anchored);
  EXPECT_EQ(clockpolicy::labelAfterBoot(false, 1000, d.store->state().anchorEpoch), TimePrecision::Unknown);
  EXPECT_EQ(clockpolicy::labelAfterBoot(true, 1000, d.store->state().anchorEpoch), TimePrecision::Exact);
}

// 7. Trust is real. The TLS configuration is the glue's; the core's part is to never hand
// it a URL that could go around it.
TEST_F(DesignRule, Rule07_TrustIsReal_CorePart_OnlyHttpsBaseUrlIsEverUsed) {
  d.fs.put(kLegacyCredentialPath, "x");
  ASSERT_EQ(d.appendRaw("a"), Outbox::AppendResult::Ok);
  manual();
  ASSERT_FALSE(d.http.log.empty());
  for (const auto& r : d.http.log) EXPECT_EQ(r.url.rfind("https://", 0), 0u) << r.url;
}

// 8. DNS chain. The chain is the glue's; the core persists the last good address and which
// host it belongs to, so a changed host cannot resurrect a stale one.
TEST_F(DesignRule, Rule08_DnsChain_CorePart_CachedIpIsStoredWithItsHost) {
  copyText(d.store->creds().serverIp, sizeof(d.store->creds().serverIp), "104.21.0.166");
  copyText(d.store->creds().serverIpHost, sizeof(d.store->creds().serverIpHost), "borges.runadev.com");
  ASSERT_TRUE(d.store->saveCredentials());
  d.crashAndBoot();
  EXPECT_STREQ(d.store->creds().serverIp, "104.21.0.166");
  EXPECT_STREQ(d.store->creds().serverIpHost, "borges.runadev.com");
}

// 9. Pull before push.
TEST_F(DesignRule, Rule09_PullBeforePush_PullFailureBlocksDrain) {
  ASSERT_EQ(d.appendRaw("stale-progress", nullptr, kMd5A, 0, EventKind::Progress), Outbox::AppendResult::Ok);
  const std::vector<Reply> failures = {apiError(503, "sync_service_unavailable"), apiError(500, "boom"),
                                       jsonReply(200, "not json")};
  for (const Reply& failure : failures) {
    d.http.log.clear();
    d.http.script.clear();
    d.http.script.push_back(failure);
    const SessionReport report = manual();
    EXPECT_TRUE(report.pullFailed);
    EXPECT_TRUE(report.drainBlockedByPull);
    EXPECT_EQ(d.http.countPath("/api/sync/v2/exchange"), 0u);
    EXPECT_FALSE(d.server.hasText("stale-progress"));
    EXPECT_EQ(d.outbox->depth(), 1u);
  }
  // Transport counts too.
  d.http.log.clear();
  d.http.script.clear();
  d.http.script.push_back(dropped());
  d.http.script.push_back(dropped());
  const SessionReport report = manual();
  EXPECT_TRUE(report.pullFailed);
  EXPECT_EQ(d.http.countPath("/api/sync/v2/exchange"), 0u);
}

TEST_F(DesignRule, Rule09_PullBeforePush_FourPagesThenTheRestNextSession) {
  for (uint64_t seq = 10; seq < 22; ++seq) d.server.addFeedEvent(seq, remoteProgress(seq, kOtherId, kMd5B, 1));
  d.config.pullLimit = 2;
  d.boot();
  EXPECT_EQ(manual().pagesPulled, 4);
  EXPECT_EQ(d.http.countPath("/api/sync/v2/pull"), 4u);
  EXPECT_EQ(d.store->state().cursor, 17u);
  d.http.log.clear();
  EXPECT_EQ(manual().pagesPulled, 2);
  EXPECT_EQ(d.store->state().cursor, 21u);
}

// 10. Cursor discipline.
TEST_F(DesignRule, Rule10_CursorDiscipline_PersistAppliedThenSendNew) {
  for (uint64_t seq = 10; seq < 14; ++seq) d.server.addFeedEvent(seq, remoteProgress(seq, kOtherId, kMd5B, static_cast<double>(seq)));
  d.config.pullLimit = 2;
  d.boot();
  std::vector<uint64_t> sentCursors;
  d.http.handler = [&](const RecordedRequest& r) {
    if (r.path == "/api/sync/v2/pull") {
      JsonDocument doc;
      deserializeJson(doc, r.body);
      const uint64_t cursor = strtoull(doc["cursor"] | "0", nullptr, 10);
      sentCursors.push_back(cursor);
      // What is on the card right now must be at least what is being sent, and the
      // head cache must already hold everything up to it.
      Store fresh(d.fs, testKey(), 6);
      EXPECT_TRUE(fresh.begin());
      EXPECT_GE(fresh.state().cursor, cursor);
      if (cursor > 0) {
        auto ledger = std::make_unique<BookLedger>();
        EXPECT_TRUE(fresh.loadBook(Device::book(kMd5B), *ledger));
        EXPECT_GE(ledger->head.serverSeq, cursor);
      }
    }
    return d.server.handle(r);
  };
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(sentCursors, (std::vector<uint64_t>{0, 11}));
  EXPECT_EQ(d.server.committed, 13u);
}

TEST_F(DesignRule, Rule10_CursorDiscipline_LastPageIsCommitted) {
  d.server.addFeedEvent(10, remoteProgress(10, kOtherId, kMd5B, 1));
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.http.countPath("/api/sync/v2/cursor/commit"), 1u);
  EXPECT_EQ(d.server.committed, 10u);
  // And nothing to commit next time.
  d.http.log.clear();
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.http.countPath("/api/sync/v2/cursor/commit"), 0u);
}

TEST_F(DesignRule, Rule10_CursorDiscipline_RespectsRegressionAndNotDelivered) {
  d.server.addFeedEvent(10, remoteProgress(10, kOtherId, kMd5B, 1));
  d.http.handler = [this](const RecordedRequest& r) {
    if (r.path == "/api/sync/v2/cursor/commit") return apiError(409, "cursor_regression", "{\"committedCursor\":\"50\"}");
    return d.server.handle(r);
  };
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.store->state().cursor, 10u) << "regression: the local checkpoint stays";

  Device fresh;
  fresh.boot();
  fresh.server.addFeedEvent(10, remoteProgress(10, kOtherId, kMd5B, 1));
  fresh.http.handler = [&fresh](const RecordedRequest& r) {
    if (r.path == "/api/sync/v2/cursor/commit") return apiError(409, "cursor_not_delivered", "{\"deliveredCursor\":\"3\"}");
    return fresh.server.handle(r);
  };
  EXPECT_EQ(fresh.engine->runManualSession(nullptr, nullptr, nullptr).outcome, SessionOutcome::Ok);
  EXPECT_EQ(fresh.store->state().cursor, 3u) << "not delivered: back to the last delivered";
}

// 11. Sequence repair.
TEST_F(DesignRule, Rule11_SequenceRepair_PersistedKeepsIdsAndOrderNeverDiscards) {
  for (int i = 1; i <= 3; ++i) d.server.usedSeq.insert(std::to_string(i));
  std::vector<std::string> ids(3);
  for (auto& id : ids) ASSERT_EQ(d.appendRaw("e", &id), Outbox::AppendResult::Ok);

  // Fail the retry, so the state after the repair can be looked at from the card.
  d.http.handler = [&](const RecordedRequest& r) {
    Reply reply = d.server.handle(r);
    if (r.path == "/api/sync/v2/exchange" && d.http.countPath(r.path) >= 2) return apiError(503, "down");
    return reply;
  };
  EXPECT_EQ(manual().outcome, SessionOutcome::ServerError);
  d.crashAndBoot();
  ASSERT_EQ(d.outbox->depth(), 3u);
  const auto live = liveEvents(*d.outbox);
  ASSERT_EQ(live.size(), 3u);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(live[i].first, ids[i]);
    EXPECT_EQ(live[i].second, 4 + i);
  }
  d.http.handler = [this](const RecordedRequest& r) { return d.server.handle(r); };
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.server.violations, std::vector<std::string>{});
}

// 12. Failure is diagnosable.
TEST_F(DesignRule, Rule12_FailureIsDiagnosable_StageCodeDetailAndRequestIdPersist) {
  Reply reply = jsonReply(503, "{\"error\":{\"code\":\"sync_service_unavailable\",\"message\":\"Borges is resting\",\"retryable\":true},\"request_id\":\"body-id\"}");
  reply.requestId = "header-id";
  d.http.script.push_back(reply);
  manual();
  d.crashAndBoot();
  const Diagnostics& diag = d.engine->diagnostics();
  EXPECT_EQ(diag.stage, SyncStage::Pull);
  EXPECT_EQ(diag.httpStatus, 503);
  EXPECT_STREQ(diag.code, "sync_service_unavailable");
  EXPECT_STREQ(diag.detail, "Borges is resting");
  EXPECT_STREQ(diag.requestId, "header-id");
  EXPECT_GT(diag.atEpoch, 0);
}

TEST_F(DesignRule, Rule12_FailureIsDiagnosable_TheRequestIdInTheBodyIsAFallback) {
  Reply reply = apiError(500, "boom");
  reply.requestId = "";
  d.http.script.push_back(reply);
  manual();
  EXPECT_STREQ(d.engine->diagnostics().requestId, "srv-req");
}

TEST_F(DesignRule, Rule12_FailureIsDiagnosable_TokensNeverReachDiagnosticsOrTheCard) {
  // Even a server that echoes the token back in its error message.
  const std::string token = "toto_SECRET_TOKEN_VALUE";
  ASSERT_EQ(d.appendRaw("a"), Outbox::AppendResult::Ok);
  d.http.script.push_back(apiError(500, "boom"));
  manual();
  const Diagnostics& diag = d.engine->diagnostics();
  EXPECT_EQ(std::string(diag.code).find("SECRET"), std::string::npos);
  EXPECT_EQ(std::string(diag.detail).find("SECRET"), std::string::npos);
  EXPECT_EQ(std::string(diag.requestId).find("SECRET"), std::string::npos);
  for (const std::string& path : d.fs.paths()) {
    EXPECT_EQ(d.fs.content(path).find(token), std::string::npos) << path;
  }
}

// 13. Wi-Fi is always turned off. The radio is the glue's; the core ends the connection
// on every path out of a session, so the glue's teardown has no half-open socket to race.
TEST_F(DesignRule, Rule13_WifiAlwaysOff_CorePart_ConnectionClosedOnEveryPath) {
  struct Path {
    const char* name;
    std::function<void(Device&)> arrange;
  };
  const std::vector<Path> paths = {
      {"success", [](Device&) {}},
      {"http error", [](Device& dev) { dev.http.script.push_back(apiError(500, "boom")); }},
      {"unauthorized", [](Device& dev) { dev.http.script.push_back(apiError(401, "invalid_credentials")); }},
      {"transport", [](Device& dev) {
         dev.http.script.push_back(dropped());
         dev.http.script.push_back(dropped());
       }},
      {"malformed", [](Device& dev) { dev.http.script.push_back(jsonReply(200, "{{{")); }},
      {"cancelled", [](Device& dev) { dev.cancel.flag = true; }},
      {"budget", [](Device& dev) { dev.config.manualBudgetMs = 0; }},
  };
  for (const Path& path : paths) {
    Device dev;
    path.arrange(dev);
    dev.boot();
    ASSERT_EQ(dev.appendRaw("a"), Outbox::AppendResult::Ok);
    dev.engine->runManualSession(nullptr, nullptr, nullptr);
    EXPECT_EQ(dev.http.disconnects >= 1, true) << path.name;
    EXPECT_FALSE(dev.http.connected) << path.name;
  }
}

// 14. Cancellable.
TEST_F(DesignRule, Rule14_Cancellable_ABeforeAnyRequestOpensTheBookAtOnce) {
  d.cancel.flag = true;
  ResumeDecision decision;
  const SessionReport report = d.engine->runOpenSession(Device::bookRef(), Device::position(1), decision);
  EXPECT_EQ(report.outcome, SessionOutcome::Cancelled);
  EXPECT_TRUE(d.http.log.empty());
  EXPECT_FALSE(decision.ask);
  // Losing nothing includes the hold: progress cannot slip out unchecked later.
  EXPECT_TRUE(d.store->state().holds.contains(Device::book().md5));
}

TEST_F(DesignRule, Rule14_Cancellable_InsideTheStream) {
  std::string events;
  for (uint64_t seq = 10; seq < 200; ++seq) {
    if (!events.empty()) events += ",";
    events += remoteProgress(seq, kOtherId, kMd5B, 1);
  }
  d.http.chunk = 256;
  d.http.script.push_back(jsonReply(200, "{\"protocol_version\":2,\"events\":[" + events + "],\"cursor\":\"199\",\"has_more\":false}"));
  d.http.advancePerRequestMs = 0;
  struct Countdown : public ICancel {
    int left;
    explicit Countdown(int n) : left(n) {}
    bool cancelled() override { return --left < 0; }
  } cancel(30);
  SyncEngine engine(*d.store, *d.outbox, d.fs, d.http, d.clock, d.random, cancel, d.config);
  ASSERT_TRUE(engine.begin());
  const SessionReport report = engine.runManualSession(nullptr, nullptr, nullptr);
  EXPECT_EQ(report.outcome, SessionOutcome::Cancelled);
  EXPECT_GT(d.http.cancelChecks, 0) << "the transfer itself was interrupted";
  EXPECT_EQ(d.store->state().cursor, 0u);
}

// 15. Bounded.
TEST_F(DesignRule, Rule15_Bounded_EachKindHasItsBudgetAndManualIgnoresBackoff) {
  d.http.handler = [](const RecordedRequest&) { return apiError(503, "down"); };
  manual();
  EXPECT_EQ(d.engine->gate(SessionKind::Open), Gate::BackedOff);
  EXPECT_EQ(d.engine->gate(SessionKind::Sleep), Gate::BackedOff);
  EXPECT_EQ(d.engine->gate(SessionKind::Manual), Gate::Go);

  for (uint8_t failures = 0; failures < 30; ++failures) {
    d.store->state().failures = failures;
    manual();
    EXPECT_LE(d.store->state().nextAttemptEpoch - d.clock.wall.epochSec, 6 * 3600);
  }
}

TEST_F(DesignRule, Rule15_Bounded_SessionStopsAtTheBudget) {
  for (int i = 0; i < 25; ++i) ASSERT_EQ(d.appendRaw("e"), Outbox::AppendResult::Ok);
  ASSERT_TRUE(d.engine->onBookOpened(Device::book()));
  d.http.advancePerRequestMs = 8000;
  const uint64_t start = d.clock.mono;
  const SessionReport report = d.engine->runSleepSession();
  EXPECT_EQ(report.outcome, SessionOutcome::BudgetExhausted);
  // Requests in flight can overrun by at most one.
  EXPECT_LE(d.clock.mono - start, d.config.sleepBudgetMs + d.http.advancePerRequestMs);
}

// 16. Non-catalog books still work.
TEST_F(DesignRule, Rule16_NonCatalogBooks_ProgressIsKeyedByHashWithANullDocument) {
  ASSERT_TRUE(d.engine->finalizeBook(d.finalizeInput(30)).progressQueued);
  manual();
  ASSERT_EQ(d.server.recorded.size(), 1u);
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, d.server.recorded[0].body));
  EXPECT_FALSE(doc["document_id"].isUnbound());
  EXPECT_TRUE(doc["document_id"].isNull());
  EXPECT_EQ(doc["book_identifier"]["kind"], "koreader_partial_md5");
  EXPECT_EQ(doc["book_identifier"]["value"], kMd5A);
  EXPECT_EQ(doc["aggregate_type"], "reading_progress");
}

TEST_F(DesignRule, Rule16_NonCatalogBooks_ReadErrorsMeanNoIdentityNeverAWrongHash) {
  MemFs fs;
  std::string content(70000, 'x');
  fs.put("/book.epub", content);
  BookId good;
  ASSERT_TRUE(partialMd5(fs, "/book.epub", good));
  EXPECT_TRUE(good.valid());

  BookId id;
  fs.failReadsAfter("/book.epub", 2000);
  EXPECT_FALSE(partialMd5(fs, "/book.epub", id));
  EXPECT_FALSE(id.valid()) << "half a read must not produce a hash";

  fs.failReadsAfter("", 0);
  fs.failSeeks("/book.epub");
  EXPECT_FALSE(partialMd5(fs, "/book.epub", id));
  EXPECT_FALSE(id.valid());

  fs.failSeeks("");
  EXPECT_FALSE(partialMd5(fs, "/missing.epub", id));
  EXPECT_FALSE(id.valid());
}

TEST_F(DesignRule, Rule16_NonCatalogBooks_ABookWithNoIdentityIsNeverQueued) {
  FinalizeInput input = d.finalizeInput(30);
  input.book.id = BookId{};
  EXPECT_TRUE(d.engine->finalizeBook(input).ok);
  EXPECT_EQ(d.outbox->depth(), 0u);
}
