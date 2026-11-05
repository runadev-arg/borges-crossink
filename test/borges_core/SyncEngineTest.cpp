#include "Backoff.h"
#include "TestSupport.h"

using namespace borges;
using namespace borges_test;

namespace {

class SyncEngineTest : public testing::Test {
 protected:
  void SetUp() override { d.boot(); }

  Device d;

  SessionReport manual() { return d.engine->runManualSession(nullptr, nullptr, nullptr); }

  SessionReport open(ResumeDecision& decision, double localPct = 20.0) {
    return d.engine->runOpenSession(Device::bookRef(), Device::position(localPct), decision);
  }

  std::vector<std::string> sentIds() const {
    std::vector<std::string> ids;
    for (const auto& r : d.http.log) {
      if (r.path != "/api/sync/v2/exchange") continue;
      JsonDocument doc;
      EXPECT_FALSE(deserializeJson(doc, r.body));
      for (JsonObject e : doc["events"].as<JsonArray>()) ids.push_back(e["client_event_id"].as<std::string>());
    }
    return ids;
  }

  std::vector<std::string> paths() const {
    std::vector<std::string> out;
    for (const auto& r : d.http.log) out.push_back(r.path);
    return out;
  }

  uint64_t lastDiskCursor_ = 0;
};

}  // namespace

TEST_F(SyncEngineTest, HappyPathPullsThenDrainsThenCommits) {
  std::string id;
  ASSERT_EQ(d.appendRaw("uno", &id), Outbox::AppendResult::Ok);
  d.server.addFeedEvent(10, remoteProgress(10, kOtherId, kMd5B, 33));

  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::Ok);
  EXPECT_EQ(report.pagesPulled, 1);
  EXPECT_EQ(report.eventsApplied, 1);
  EXPECT_EQ(report.eventsAcked, 1);
  EXPECT_EQ(report.depth, 0u);
  EXPECT_TRUE(d.server.hasText("uno"));
  EXPECT_EQ(d.server.violations, std::vector<std::string>{});

  // Order: pull, then capabilities and exchange, then commit of the last cursor.
  const auto seen = paths();
  ASSERT_GE(seen.size(), 4u);
  EXPECT_EQ(seen[0], "/api/sync/v2/pull");
  EXPECT_EQ(seen[1], "/api/sync/v2/capabilities");
  EXPECT_EQ(seen[2], "/api/sync/v2/exchange");
  EXPECT_EQ(d.http.countPath("/api/sync/v2/cursor/commit"), 1u);
  EXPECT_EQ(d.store->state().cursor, d.store->state().committedCursor);
  EXPECT_EQ(d.server.committed, d.store->state().cursor);
  EXPECT_EQ(d.http.log[0].headers.at("Authorization"), "Bearer toto_SECRET_TOKEN_VALUE");
  EXPECT_EQ(d.http.log[0].url.rfind("https://borges.runadev.com/api/", 0), 0u);
}

TEST_F(SyncEngineTest, ClientBlockMatchesTheProtocol) {
  ASSERT_EQ(d.appendRaw("uno"), Outbox::AppendResult::Ok);
  manual();
  bool sawExchange = false;
  for (const auto& r : d.http.log) {
    if (r.path != "/api/sync/v2/exchange") continue;
    sawExchange = true;
    JsonDocument doc;
    ASSERT_FALSE(deserializeJson(doc, r.body));
    EXPECT_EQ(doc["protocol_version"], 2);
    EXPECT_TRUE(doc["cursor"].is<const char*>());
    EXPECT_EQ(doc["client"]["protocol_version"], 2);
    EXPECT_EQ(doc["client"]["client_version"], "crossink-borges/1.6.1");
    EXPECT_EQ(doc["client"]["firmware_version"], "1.6.1");
    EXPECT_EQ(doc["client"]["queue_depth"], 1);
    EXPECT_EQ(doc["client"]["capabilities"]["atomic_sd_outbox"], true);
    EXPECT_TRUE(doc["events"][0]["client_sequence"].is<const char*>());
    EXPECT_EQ(doc["events"][0]["event_version"], 1);
  }
  EXPECT_TRUE(sawExchange);
}

TEST_F(SyncEngineTest, EveryPullTellsTheServerTheQueueDepth) {
  ASSERT_EQ(d.appendRaw("held"), Outbox::AppendResult::Ok);
  d.store->state().holds.add(Device::book().md5);
  ResumeDecision decision;
  ASSERT_EQ(open(decision).outcome, SessionOutcome::Ok);
  size_t pulls = 0;
  for (const auto& r : d.http.log) {
    if (r.path != "/api/sync/v2/pull") continue;
    ++pulls;
    JsonDocument doc;
    ASSERT_FALSE(deserializeJson(doc, r.body)) << r.body;
    EXPECT_EQ(doc["client"]["queue_depth"], 1) << r.body;
    EXPECT_EQ(doc["client"]["client_version"], "crossink-borges/1.6.1");
  }
  EXPECT_EQ(pulls, 2u) << "the focus check and the pull";
}

// --- Exchange outcomes ------------------------------------------------------------

TEST_F(SyncEngineTest, PartialRejectionsLeaveTheOutboxAndAreCounted) {
  ASSERT_EQ(d.appendRaw("good-1"), Outbox::AppendResult::Ok);
  ASSERT_EQ(d.appendRaw("bad"), Outbox::AppendResult::Ok);
  ASSERT_EQ(d.appendRaw("good-2"), Outbox::AppendResult::Ok);
  d.server.rejectText.insert("bad");
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::Ok);
  EXPECT_EQ(report.eventsAcked, 2);
  EXPECT_EQ(report.eventsRejected, 1);
  EXPECT_EQ(report.depth, 0u);
  EXPECT_EQ(d.store->state().rejectedEvents, 1u);
  EXPECT_STREQ(d.store->state().lastRejection, "nope");
  // Nothing comes back after a reboot, rejected or not.
  d.crashAndBoot();
  EXPECT_EQ(d.outbox->depth(), 0u);
}

TEST_F(SyncEngineTest, RetransmissionAfterLostResponseIsTheSameEvent) {
  std::string id;
  ASSERT_EQ(d.appendRaw("uno", &id), Outbox::AppendResult::Ok);
  // The server commits, and the response is lost on the way back.
  d.http.handler = [this](const RecordedRequest& r) {
    Reply reply = d.server.handle(r);
    if (r.path == "/api/sync/v2/exchange" && d.http.countPath(r.path) == 1) reply.dropAfterBytes = 10;
    return reply;
  };
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::Ok);
  EXPECT_EQ(report.depth, 0u);
  EXPECT_EQ(d.server.recorded.size(), 1u);
  EXPECT_EQ(d.server.duplicates, 1);
  EXPECT_EQ(sentIds(), (std::vector<std::string>{id, id}));
  EXPECT_EQ(d.server.violations, std::vector<std::string>{});
}

TEST_F(SyncEngineTest, LostResponseAcrossAPowerCutIsStillTheSameEvent) {
  std::string id;
  ASSERT_EQ(d.appendRaw("uno", &id), Outbox::AppendResult::Ok);
  // Two drops use up the one reconnect, so the session fails.
  d.http.script.push_back(jsonReply(200, FakeServer::capabilities().body));
  d.http.handler = [this](const RecordedRequest& r) {
    Reply reply = d.server.handle(r);
    if (r.path == "/api/sync/v2/exchange" && d.http.countPath(r.path) <= 2) reply.dropAfterBytes = 3;
    return reply;
  };
  d.http.script.clear();
  const SessionReport first = manual();
  EXPECT_EQ(first.outcome, SessionOutcome::Transport);
  EXPECT_EQ(d.outbox->depth(), 1u);

  d.crashAndBoot();
  d.http.handler = [this](const RecordedRequest& r) { return d.server.handle(r); };
  const SessionReport second = manual();
  EXPECT_EQ(second.outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.server.recorded.size(), 1u);
  for (const std::string& seen : sentIds()) EXPECT_EQ(seen, id);
  EXPECT_EQ(d.server.violations, std::vector<std::string>{});
}

TEST_F(SyncEngineTest, MissingAcksKeepTheEventAndReportIt) {
  ASSERT_EQ(d.appendRaw("uno"), Outbox::AppendResult::Ok);
  d.http.handler = [this](const RecordedRequest& r) {
    if (r.path == "/api/sync/v2/exchange") {
      return jsonReply(200, "{\"protocol_version\":2,\"acknowledgements\":[],\"pull\":{\"events\":[],\"cursor\":\"0\",\"has_more\":false}}");
    }
    return d.server.handle(r);
  };
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::ProtocolError);
  EXPECT_STREQ(d.store->state().diag.code, "ack_missing");
  EXPECT_EQ(d.outbox->depth(), 1u);
}

TEST_F(SyncEngineTest, AckWithAnotherSequenceIsNotTrusted) {
  std::string id;
  ASSERT_EQ(d.appendRaw("uno", &id), Outbox::AppendResult::Ok);
  d.http.handler = [this, id](const RecordedRequest& r) {
    if (r.path == "/api/sync/v2/exchange") {
      return jsonReply(200, "{\"protocol_version\":2,\"acknowledgements\":[{\"client_event_id\":\"" + id +
                                "\",\"client_sequence\":\"999\",\"status\":\"accepted\"}],\"pull\":{\"events\":[],\"cursor\":\"0\",\"has_more\":false}}");
    }
    return d.server.handle(r);
  };
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::ProtocolError);
  EXPECT_STREQ(d.store->state().diag.code, "ack_sequence_mismatch");
  EXPECT_EQ(d.outbox->depth(), 1u);
}

TEST_F(SyncEngineTest, UnknownAckStatusIsNotTrusted) {
  std::string id;
  ASSERT_EQ(d.appendRaw("uno", &id), Outbox::AppendResult::Ok);
  d.http.handler = [this, id](const RecordedRequest& r) {
    if (r.path == "/api/sync/v2/exchange") {
      return jsonReply(200, "{\"protocol_version\":2,\"acknowledgements\":[{\"client_event_id\":\"" + id +
                                "\",\"client_sequence\":\"1\",\"status\":\"maybe\"}],\"pull\":{\"events\":[],\"cursor\":\"0\",\"has_more\":false}}");
    }
    return d.server.handle(r);
  };
  EXPECT_EQ(manual().outcome, SessionOutcome::ProtocolError);
  EXPECT_EQ(d.outbox->depth(), 1u);
}

TEST_F(SyncEngineTest, ValidationFailureSplitsTheBatchAndQuarantinesOnlyTheCulprit) {
  ASSERT_EQ(d.appendRaw("good-1"), Outbox::AppendResult::Ok);
  ASSERT_EQ(d.appendRaw("culprit"), Outbox::AppendResult::Ok);
  ASSERT_EQ(d.appendRaw("good-2"), Outbox::AppendResult::Ok);
  d.http.handler = [this](const RecordedRequest& r) {
    if (r.path == "/api/sync/v2/exchange" && r.body.find("culprit") != std::string::npos) {
      return apiError(422, "validation_failed", "[{\"code\":\"invalid_uuid\",\"path\":\"events[0].client_event_id\"}]");
    }
    return d.server.handle(r);
  };
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::Ok);
  EXPECT_EQ(report.eventsQuarantined, 1);
  EXPECT_EQ(report.depth, 0u);
  EXPECT_TRUE(d.server.hasText("good-1"));
  EXPECT_TRUE(d.server.hasText("good-2"));
  EXPECT_FALSE(d.server.hasText("culprit"));
  // Set aside on the card, never discarded.
  EXPECT_NE(d.fs.content(std::string(kOutboxDir) + "/outbox.bad").find("culprit"), std::string::npos);
  EXPECT_STREQ(d.store->state().lastRejection, "validation_failed");
}

TEST_F(SyncEngineTest, ValidationFailureAboutTheRequestDoesNotTouchTheQueue) {
  ASSERT_EQ(d.appendRaw("a"), Outbox::AppendResult::Ok);
  ASSERT_EQ(d.appendRaw("b"), Outbox::AppendResult::Ok);
  d.http.handler = [this](const RecordedRequest& r) {
    if (r.path == "/api/sync/v2/exchange") {
      return apiError(422, "validation_failed", "[{\"code\":\"invalid_client_version\",\"path\":\"client.firmware_version\"}]");
    }
    return d.server.handle(r);
  };
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::ServerError);
  EXPECT_EQ(report.eventsQuarantined, 0);
  EXPECT_EQ(d.outbox->depth(), 2u);
  EXPECT_FALSE(d.fs.has(std::string(kOutboxDir) + "/outbox.bad"));
}

TEST_F(SyncEngineTest, QuarantinePerSessionIsCappedSoAWrongServerCannotEmptyTheQueue) {
  for (int i = 0; i < 10; ++i) ASSERT_EQ(d.appendRaw("e"), Outbox::AppendResult::Ok);
  d.http.handler = [this](const RecordedRequest& r) {
    if (r.path == "/api/sync/v2/exchange") return apiError(409, "idempotency_conflict");
    return d.server.handle(r);
  };
  const SessionReport report = manual();
  EXPECT_EQ(report.eventsQuarantined, 4);
  EXPECT_EQ(report.outcome, SessionOutcome::ServerError);
  EXPECT_EQ(d.outbox->depth(), 6u);
}

TEST_F(SyncEngineTest, RequestTooLargeSplitsTheBatch) {
  ASSERT_EQ(d.appendRaw("a"), Outbox::AppendResult::Ok);
  ASSERT_EQ(d.appendRaw("b"), Outbox::AppendResult::Ok);
  d.http.handler = [this](const RecordedRequest& r) {
    if (r.path == "/api/sync/v2/exchange") {
      JsonDocument doc;
      deserializeJson(doc, r.body);
      if (doc["events"].size() > 1) return apiError(413, "request_too_large");
    }
    return d.server.handle(r);
  };
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::Ok);
  EXPECT_EQ(report.eventsQuarantined, 0);
  EXPECT_TRUE(d.server.hasText("a"));
  EXPECT_TRUE(d.server.hasText("b"));
}

TEST_F(SyncEngineTest, IdempotencyConflictDoesNotMutateTheEvent) {
  std::string id;
  ASSERT_EQ(d.appendRaw("uno", &id), Outbox::AppendResult::Ok);
  d.http.handler = [this](const RecordedRequest& r) {
    if (r.path == "/api/sync/v2/exchange") return apiError(409, "idempotency_conflict");
    return d.server.handle(r);
  };
  const SessionReport report = manual();
  EXPECT_EQ(report.eventsQuarantined, 1);
  EXPECT_NE(d.fs.content(std::string(kOutboxDir) + "/outbox.bad").find(R"("text":"uno")"), std::string::npos);
  EXPECT_EQ(sentIds(), std::vector<std::string>{id});
}

// --- Sequence repair --------------------------------------------------------------

TEST_F(SyncEngineTest, SequenceConflictIsRepairedPersistedAndRetriedOnce) {
  // A re-paired device: the server's counter is ahead of this fresh one.
  for (int i = 1; i <= 5; ++i) d.server.usedSeq.insert(std::to_string(i));
  std::string a, b;
  ASSERT_EQ(d.appendRaw("a", &a), Outbox::AppendResult::Ok);
  ASSERT_EQ(d.appendRaw("b", &b), Outbox::AppendResult::Ok);

  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::Ok);
  EXPECT_TRUE(report.sequenceRepaired);
  EXPECT_EQ(report.depth, 0u);
  // Same ids both times, same order.
  EXPECT_EQ(sentIds(), (std::vector<std::string>{a, b, a, b}));
  ASSERT_EQ(d.server.recorded.size(), 2u);
  EXPECT_EQ(d.server.recorded[0].id, a);
  EXPECT_EQ(d.server.recorded[0].seq, "6");
  EXPECT_EQ(d.server.recorded[1].id, b);
  EXPECT_EQ(d.server.recorded[1].seq, "7");
  EXPECT_EQ(d.outbox->nextSequence(), 8u);
  EXPECT_EQ(d.server.violations, std::vector<std::string>{});
}

TEST_F(SyncEngineTest, RepairedSequencesSurviveALostRetryResponse) {
  for (int i = 1; i <= 5; ++i) d.server.usedSeq.insert(std::to_string(i));
  std::string a;
  ASSERT_EQ(d.appendRaw("a", &a), Outbox::AppendResult::Ok);
  // Retry reaches the server, then the connection dies twice.
  d.http.handler = [this](const RecordedRequest& r) {
    Reply reply = d.server.handle(r);
    if (r.path == "/api/sync/v2/exchange" && d.http.countPath(r.path) >= 2) reply.dropAfterBytes = 0;
    return reply;
  };
  EXPECT_EQ(manual().outcome, SessionOutcome::Transport);
  d.crashAndBoot();
  EXPECT_EQ(d.outbox->entryAt(0)->seq, 6u);
  d.http.handler = [this](const RecordedRequest& r) { return d.server.handle(r); };
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  ASSERT_EQ(d.server.recorded.size(), 1u);
  EXPECT_EQ(d.server.recorded[0].seq, "6");
  EXPECT_EQ(d.server.violations, std::vector<std::string>{});
}

TEST_F(SyncEngineTest, SecondConflictAfterRepairStopsInsteadOfLooping) {
  std::string a;
  ASSERT_EQ(d.appendRaw("a", &a), Outbox::AppendResult::Ok);
  int conflicts = 0;
  d.http.handler = [&](const RecordedRequest& r) {
    if (r.path == "/api/sync/v2/exchange") {
      ++conflicts;
      JsonDocument doc;
      deserializeJson(doc, r.body);
      const std::string seq = doc["events"][0]["client_sequence"].as<std::string>();
      return apiError(409, "client_sequence_conflict",
                      "{\"next_client_sequence\":\"50\",\"unrecorded_events\":[{\"client_event_id\":\"" + a +
                          "\",\"client_sequence\":\"" + seq + "\"}]}");
    }
    return d.server.handle(r);
  };
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::ServerError);
  EXPECT_EQ(conflicts, 2);
  EXPECT_EQ(d.outbox->depth(), 1u);
  EXPECT_STREQ(d.store->state().diag.code, "client_sequence_conflict");
}

TEST_F(SyncEngineTest, ConflictWithoutProofChangesNothing) {
  std::string a;
  ASSERT_EQ(d.appendRaw("a", &a), Outbox::AppendResult::Ok);
  const std::vector<std::string> proofs = {
      "",  // No details at all.
      "{\"next_client_sequence\":\"9\"}",
      "{\"next_client_sequence\":\"9\",\"unrecorded_events\":[]}",
      "{\"unrecorded_events\":[{\"client_event_id\":\"" + a + "\",\"client_sequence\":\"1\"}]}",
      // Not something we sent.
      "{\"next_client_sequence\":\"9\",\"unrecorded_events\":[{\"client_event_id\":\"30000000-0000-4000-8000-000000000009\",\"client_sequence\":\"1\"}]}",
      // Our id under a number we did not send.
      "{\"next_client_sequence\":\"9\",\"unrecorded_events\":[{\"client_event_id\":\"" + a + "\",\"client_sequence\":\"2\"}]}",
      "{\"next_client_sequence\":\"nine\",\"unrecorded_events\":[{\"client_event_id\":\"" + a + "\",\"client_sequence\":\"1\"}]}",
  };
  for (const std::string& proof : proofs) {
    d.http.log.clear();
    d.http.handler = [&](const RecordedRequest& r) {
      if (r.path == "/api/sync/v2/exchange") return apiError(409, "client_sequence_conflict", proof);
      return d.server.handle(r);
    };
    const SessionReport report = manual();
    EXPECT_EQ(report.outcome, SessionOutcome::ServerError) << proof;
    EXPECT_FALSE(report.sequenceRepaired) << proof;
    ASSERT_EQ(d.outbox->depth(), 1u);
    EXPECT_EQ(d.outbox->entryAt(0)->seq, 1u) << proof;
    EXPECT_EQ(d.http.countPath("/api/sync/v2/exchange"), 1u) << proof;
  }
}

// --- Cursor discipline ------------------------------------------------------------

TEST_F(SyncEngineTest, CursorIsPersistedBeforeItIsSent) {
  d.server.addFeedEvent(10, remoteProgress(10, kOtherId, kMd5B, 33));
  d.server.addFeedEvent(11, remoteProgress(11, kOtherId, kMd5B, 34));
  d.server.pageLimit = 1;
  d.config.pullLimit = 1;
  d.boot();
  // Every request's cursor must already be on disk when it goes out.
  d.http.beforeRequest = [this](size_t) {
    Store fresh(d.fs, testKey(), 6, testBox());
    fresh.begin();
    lastDiskCursor_ = fresh.state().cursor;
  };
  d.http.handler = [this](const RecordedRequest& r) {
    if (r.path == "/api/sync/v2/pull" || r.path == "/api/sync/v2/cursor/commit") {
      JsonDocument doc;
      deserializeJson(doc, r.body);
      const uint64_t sent = strtoull(doc["cursor"] | "0", nullptr, 10);
      EXPECT_LE(sent, lastDiskCursor_) << r.path;
    }
    return d.server.handle(r);
  };
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.store->state().cursor, 11u);
}

TEST_F(SyncEngineTest, CommitRegressionKeepsTheLocalCheckpointAndKeepsWorking) {
  std::string id;
  ASSERT_EQ(d.appendRaw("uno", &id), Outbox::AppendResult::Ok);
  d.server.addFeedEvent(10, remoteProgress(10, kOtherId, kMd5B, 33));
  d.server.delivered = 10;
  d.server.committed = 50;  // The SD card was restored from an older copy.
  d.server.nextServerSeq = 20;
  d.http.handler = [this](const RecordedRequest& r) {
    if (r.path == "/api/sync/v2/cursor/commit") {
      return apiError(409, "cursor_regression", "{\"cursor\":\"10\",\"committedCursor\":\"50\"}");
    }
    return d.server.handle(r);
  };
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::Ok);
  EXPECT_TRUE(report.cursorRecovered);
  EXPECT_EQ(report.depth, 0u);
  // Never jump to the server's checkpoint: that would skip what was never applied.
  EXPECT_LT(d.store->state().cursor, 50u);
  EXPECT_EQ(d.store->state().failures, 0);
}

TEST_F(SyncEngineTest, PullNotDeliveredMovesTheCursorBackAndRetriesOnce) {
  d.store->state().cursor = 90;
  ASSERT_TRUE(d.store->saveState());
  d.server.delivered = 40;
  d.server.addFeedEvent(45, remoteProgress(45, kOtherId, kMd5B, 33));
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::Ok);
  EXPECT_TRUE(report.cursorRecovered);
  EXPECT_EQ(report.eventsApplied, 1);
  EXPECT_EQ(d.store->state().cursor, 45u);
}

TEST_F(SyncEngineTest, NotDeliveredNeverMovesTheCursorForward) {
  d.store->state().cursor = 10;
  ASSERT_TRUE(d.store->saveState());
  d.http.handler = [](const RecordedRequest& r) {
    if (r.path == "/api/sync/v2/pull") return apiError(409, "cursor_not_delivered", "{\"cursor\":\"10\",\"deliveredCursor\":\"500\"}");
    return jsonReply(200, "{}");
  };
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::ServerError);
  EXPECT_EQ(d.store->state().cursor, 10u);
}

TEST_F(SyncEngineTest, CommitNotDeliveredMovesBackToDelivered) {
  d.server.addFeedEvent(10, remoteProgress(10, kOtherId, kMd5B, 33));
  d.http.handler = [this](const RecordedRequest& r) {
    if (r.path == "/api/sync/v2/cursor/commit") return apiError(409, "cursor_not_delivered", "{\"cursor\":\"10\",\"deliveredCursor\":\"4\"}");
    return d.server.handle(r);
  };
  const SessionReport report = manual();
  EXPECT_TRUE(report.cursorRecovered);
  EXPECT_EQ(d.store->state().cursor, 4u);
}

TEST_F(SyncEngineTest, CursorThatGoesBackwardsInAResponseIsMalformed) {
  d.store->state().cursor = 90;
  ASSERT_TRUE(d.store->saveState());
  d.http.script.push_back(jsonReply(200, "{\"protocol_version\":2,\"events\":[],\"cursor\":\"5\",\"has_more\":false}"));
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::ProtocolError);
  EXPECT_EQ(d.store->state().cursor, 90u);
}

// --- Paging -----------------------------------------------------------------------

TEST_F(SyncEngineTest, HasMoreStopsAtFourPagesAndContinuesNextSession) {
  std::string id;
  ASSERT_EQ(d.appendRaw("uno", &id), Outbox::AppendResult::Ok);
  for (uint64_t seq = 10; seq < 20; ++seq) d.server.addFeedEvent(seq, remoteProgress(seq, kOtherId, kMd5B, 1));
  d.config.pullLimit = 1;
  d.boot();

  SessionReport first = manual();
  EXPECT_EQ(first.pagesPulled, 4);
  EXPECT_EQ(first.outcome, SessionOutcome::Ok);
  EXPECT_EQ(first.depth, 0u);  // The drain still ran.
  // Pull pages, then the drain's own exchange brought one more page.
  EXPECT_EQ(d.store->state().cursor, 14u);

  SessionReport second = manual();
  EXPECT_EQ(second.pagesPulled, 4);
  EXPECT_EQ(d.store->state().cursor, 18u);
  SessionReport third = manual();
  EXPECT_EQ(third.pagesPulled, 1);
  // The last page ends at the high watermark, which includes our own uploaded event.
  EXPECT_EQ(d.store->state().cursor, d.server.highWatermark());
}

TEST_F(SyncEngineTest, HasMoreWithoutProgressDoesNotSpin) {
  d.http.handler = [](const RecordedRequest&) {
    return jsonReply(200, "{\"protocol_version\":2,\"events\":[],\"cursor\":\"0\",\"has_more\":true}");
  };
  const SessionReport report = manual();
  EXPECT_EQ(report.pagesPulled, 1);
  EXPECT_EQ(d.http.countPath("/api/sync/v2/pull"), 1u);
}

TEST_F(SyncEngineTest, EmptyPageCanStillAdvanceTheCursor) {
  d.server.delivered = 1u << 30;  // These replies are scripted, past the model server.
  d.http.script.push_back(jsonReply(200, "{\"protocol_version\":2,\"events\":[],\"cursor\":\"77\",\"high_watermark\":\"77\",\"has_more\":false}"));
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.store->state().cursor, 77u);
}

TEST_F(SyncEngineTest, PullLimitIsWithinTheServersBound) {
  manual();
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, d.http.log[0].body));
  EXPECT_GE(doc["limit"].as<int>(), 1);
  EXPECT_LE(doc["limit"].as<int>(), 20);
}

// --- Failure classes --------------------------------------------------------------

TEST_F(SyncEngineTest, Unauthorized401StopsAutomaticSessionsAndKeepsTheQueue) {
  ASSERT_EQ(d.appendRaw("uno"), Outbox::AppendResult::Ok);
  d.http.handler = [](const RecordedRequest&) { return apiError(401, "invalid_credentials"); };
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::NeedsRelink);
  EXPECT_EQ(d.outbox->depth(), 1u);
  EXPECT_TRUE(d.store->state().needsRelink);
  EXPECT_STREQ(d.store->state().diag.code, "invalid_credentials");

  EXPECT_EQ(d.engine->gate(SessionKind::Open), Gate::NeedsRelink);
  EXPECT_EQ(d.engine->gate(SessionKind::Sleep), Gate::NeedsRelink);
  EXPECT_EQ(d.engine->gate(SessionKind::Manual), Gate::Go);

  d.crashAndBoot();
  EXPECT_EQ(d.engine->gate(SessionKind::Open), Gate::NeedsRelink);
  EXPECT_EQ(d.outbox->depth(), 1u);
  // Not a backoff case: waiting does not heal it.
  EXPECT_EQ(d.store->state().failures, 0);
}

TEST_F(SyncEngineTest, Revoked403IsTheSameAs401) {
  d.http.handler = [](const RecordedRequest&) { return apiError(403, "device_revoked"); };
  EXPECT_EQ(manual().outcome, SessionOutcome::NeedsRelink);
  EXPECT_TRUE(d.store->state().needsRelink);
}

TEST_F(SyncEngineTest, ManualSuccessAfter401ClearsIt) {
  d.http.script.push_back(apiError(401, "invalid_credentials"));
  EXPECT_EQ(manual().outcome, SessionOutcome::NeedsRelink);
  d.http.script.clear();
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_FALSE(d.store->state().needsRelink);
  EXPECT_EQ(d.engine->gate(SessionKind::Open), Gate::Go);
}

TEST_F(SyncEngineTest, RateLimit429HonoursRetryAfter) {
  Reply reply = apiError(429, "rate_limited");
  reply.hasRetryAfter = true;
  reply.retryAfter = 3600;
  d.http.script.push_back(reply);
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::RateLimited);
  EXPECT_GE(d.store->state().nextAttemptEpoch, kNow + 3600);
  EXPECT_EQ(d.engine->gate(SessionKind::Open), Gate::BackedOff);
  EXPECT_EQ(d.engine->gate(SessionKind::Sleep), Gate::BackedOff);
  EXPECT_EQ(d.engine->gate(SessionKind::Manual), Gate::Go);
  d.clock.wall.epochSec = kNow + 3601;
  EXPECT_EQ(d.engine->gate(SessionKind::Open), Gate::Go);
}

TEST_F(SyncEngineTest, AHugeRetryAfterCannotSilenceTheDeviceBeyondTheCap) {
  Reply reply = apiError(429, "rate_limited");
  reply.hasRetryAfter = true;
  reply.retryAfter = 0xFFFFFFFFu;
  d.http.script.push_back(reply);
  EXPECT_EQ(manual().outcome, SessionOutcome::RateLimited);
  EXPECT_LE(d.store->state().nextAttemptEpoch, kNow + backoff::kCapSec);
  d.clock.wall.epochSec = kNow + backoff::kCapSec;
  EXPECT_EQ(d.engine->gate(SessionKind::Open), Gate::Go);
}

TEST_F(SyncEngineTest, AServerTimeOutOfBoundsMovesNeitherTheClockNorTheAnchor) {
  d.clock.setWall(0, TimePrecision::Unknown);
  d.clock.setCalls = 0;
  d.store->state().anchorEpoch = kNow - 100;
  d.http.script.push_back(jsonReply(200, "{\"protocol_version\":2,\"events\":[],\"cursor\":\"0\",\"has_more\":false,"
                                         "\"server_time\":\"1999-01-01T00:00:00.000Z\"}"));
  manual();
  EXPECT_EQ(d.clock.setCalls, 0) << "not even from the server: a date out of bounds is not a date";
  EXPECT_EQ(d.store->state().anchorEpoch, kNow - 100);
}

TEST_F(SyncEngineTest, HeldEventsDoNotPowerTheRadioAtEverySleep) {
  // A book that wants a prompt, closed unanswered, with its progress held.
  d.store->state().holds.add(Device::book().md5);
  FinalizeInput input = d.finalizeInput(30);
  ASSERT_TRUE(d.engine->finalizeBook(input).progressQueued);
  d.server.focusEvent = focusEventJson(kOtherId, kMd5A, 60);
  EXPECT_EQ(d.engine->gate(SessionKind::Sleep), Gate::Go) << "the first sleep asks, once";
  EXPECT_EQ(d.engine->runSleepSession().outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.http.countPath("/api/sync/v2/exchange"), 0u) << "still held";
  EXPECT_EQ(d.engine->gate(SessionKind::Sleep), Gate::NothingToDo) << "nothing new to ask";

  // It survives a reboot, or every wake would ask again.
  d.crashAndBoot();
  EXPECT_EQ(d.engine->gate(SessionKind::Sleep), Gate::NothingToDo);

  // Reading it again is news.
  FinalizeInput later = d.finalizeInput(35, "/body/p[2].0", kNow + 60);
  ASSERT_TRUE(d.engine->finalizeBook(later).progressQueued);
  EXPECT_EQ(d.engine->gate(SessionKind::Sleep), Gate::Go);

  // Something that can actually go up is always work.
  d.crashAndBoot();
  d.server.focusEvent = "null";
  EXPECT_EQ(d.engine->runSleepSession().outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.store->state().holds.size(), 0);
  ASSERT_EQ(d.appendRaw("uno"), Outbox::AppendResult::Ok);
  EXPECT_EQ(d.engine->gate(SessionKind::Sleep), Gate::Go);
}

TEST_F(SyncEngineTest, HoldSetKeepsTheCheckedMarksThroughItsShifts) {
  HoldSet holds;
  uint8_t a[16] = {1}, b[16] = {2}, c[16] = {3};
  holds.add(a);
  holds.add(b);
  holds.add(c);
  holds.markChecked(a);
  holds.markChecked(c);
  holds.remove(b);
  EXPECT_TRUE(holds.checked(0));
  EXPECT_TRUE(holds.checked(1));
  EXPECT_TRUE(holds.clearChecked(c));
  EXPECT_FALSE(holds.clearChecked(c));
  EXPECT_FALSE(holds.checked(1));
}

TEST_F(SyncEngineTest, TwoLinkFailuresSkipTheNextFiveAutomaticAttempts) {
  ASSERT_EQ(d.appendRaw("uno"), Outbox::AppendResult::Ok);
  EXPECT_EQ(d.engine->decideAutomatic(SessionKind::Open), Gate::Go);
  d.engine->noteLinkFailure();
  EXPECT_EQ(d.engine->decideAutomatic(SessionKind::Open), Gate::Go) << "one failure is not a pattern";
  d.engine->noteLinkFailure();
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(d.engine->decideAutomatic(i % 2 ? SessionKind::Open : SessionKind::Sleep), Gate::BackedOff) << i;
  }
  EXPECT_EQ(d.engine->gate(SessionKind::Manual), Gate::Go);
  EXPECT_EQ(d.engine->decideAutomatic(SessionKind::Open), Gate::Go);
  // Still failing: another five, not one more try each time.
  d.engine->noteLinkFailure();
  EXPECT_EQ(d.engine->gate(SessionKind::Open), Gate::BackedOff);
}

TEST_F(SyncEngineTest, LinkSkipsDoNotDependOnTheClockAndSurviveAReboot) {
  d.clock.setWall(0, TimePrecision::Unknown);
  d.engine->noteLinkFailure();
  d.engine->noteLinkFailure();
  d.crashAndBoot();
  d.clock.setWall(0, TimePrecision::Unknown);
  EXPECT_EQ(d.engine->gate(SessionKind::Open), Gate::BackedOff);
  EXPECT_EQ(d.engine->decideAutomatic(SessionKind::Open), Gate::BackedOff);
  EXPECT_EQ(d.store->state().linkSkipsLeft, 4);
}

TEST_F(SyncEngineTest, AnySessionWithALinkClearsTheSkips) {
  d.engine->noteLinkFailure();
  d.engine->noteLinkFailure();
  ASSERT_EQ(d.engine->gate(SessionKind::Open), Gate::BackedOff);
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.store->state().linkSkipsLeft, 0);
  EXPECT_EQ(d.store->state().linkFailStreak, 0);
  EXPECT_EQ(d.engine->gate(SessionKind::Open), Gate::Go);
}

TEST_F(SyncEngineTest, ASleepWithNothingToSendDoesNotSpendALinkSkip) {
  d.engine->noteLinkFailure();
  d.engine->noteLinkFailure();
  EXPECT_EQ(d.engine->decideAutomatic(SessionKind::Sleep), Gate::NothingToDo);
  EXPECT_EQ(d.store->state().linkSkipsLeft, 5);
}

TEST_F(SyncEngineTest, ServerErrorsBackOffExponentiallyAndSuccessResets) {
  d.http.handler = [](const RecordedRequest&) { return apiError(503, "sync_service_unavailable"); };
  int64_t previous = 0;
  for (uint8_t n = 0; n < 5; ++n) {
    EXPECT_EQ(manual().outcome, SessionOutcome::ServerError);
    EXPECT_EQ(d.store->state().failures, n + 1);
    const int64_t delay = d.store->state().nextAttemptEpoch - kNow;
    const int64_t base = 15LL << n;
    EXPECT_GE(delay, base * 8 / 10);
    EXPECT_LE(delay, base * 12 / 10);
    EXPECT_GT(delay, previous / 2);
    previous = delay;
  }
  EXPECT_EQ(d.engine->gate(SessionKind::Open), Gate::BackedOff);
  d.http.handler = [this](const RecordedRequest& r) { return d.server.handle(r); };
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.store->state().failures, 0);
  EXPECT_EQ(d.store->state().nextAttemptEpoch, 0);
  EXPECT_FALSE(d.store->state().diag.hasError());
  EXPECT_EQ(d.engine->gate(SessionKind::Open), Gate::Go);
}

TEST_F(SyncEngineTest, BackoffSurvivesAReboot) {
  d.http.handler = [](const RecordedRequest&) { return apiError(500, "boom"); };
  manual();
  d.crashAndBoot();
  EXPECT_EQ(d.store->state().failures, 1);
  EXPECT_EQ(d.engine->gate(SessionKind::Sleep), Gate::BackedOff);
}

TEST_F(SyncEngineTest, SocketDropThenReconnectSucceeds) {
  d.http.script.push_back(dropped());
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.http.connections, 2);  // The one reconnect, then the session's connection held.
  EXPECT_EQ(d.http.disconnects, 2);  // Before the reconnect, and at the end of the session.
}

TEST_F(SyncEngineTest, SecondDropInASessionFailsWithTransport) {
  d.http.script.push_back(dropped());
  d.http.script.push_back(dropped());
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::Transport);
  EXPECT_EQ(d.http.log.size(), 2u);
  EXPECT_STREQ(d.store->state().diag.code, "transport");
  EXPECT_EQ(d.store->state().diag.transportStage, TransportStage::ReadHeaders);
  EXPECT_EQ(d.store->state().diag.transportCode, -7);
  EXPECT_GT(d.store->state().nextAttemptEpoch, kNow);
}

TEST_F(SyncEngineTest, BodyCutHalfwayCountsAsADrop) {
  Reply cut = jsonReply(200, "{\"protocol_version\":2,\"events\":[],\"cursor\":\"5\",\"has_more\":false}");
  cut.dropAfterBytes = 12;
  d.http.script.push_back(cut);
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.store->state().cursor, 0u);  // Not the cut one's cursor.
  EXPECT_EQ(d.http.connections, 2);
}

// --- Malformed and poison ---------------------------------------------------------

TEST_F(SyncEngineTest, MalformedPullChangesNothingAndBlocksTheDrain) {
  const std::vector<std::string> bodies = {
      "",
      "not json",
      "{\"protocol_version\":2,\"events\":[",
      "{\"protocol_version\":2,\"events\":[],\"cursor\":\"5\",\"has_more\":false} trailing",
      "{\"protocol_version\":2,\"events\":[{\"a\":}],\"cursor\":\"5\"}",
      "{\"protocol_version\":2,\"events\":\"nope\",\"cursor\":\"5\"}",
      "{\"protocol_version\":2,\"events\":[]}",
      "{\"protocol_version\":2,\"events\":[],\"cursor\":5}",
      "{\"protocol_version\":2,\"events\":[],\"cursor\":\"-5\"}",
      "{\"protocol_version\":2,\"events\":[],\"cursor\":\"99999999999999999999\"}",
      "{\"protocol_version\":3,\"events\":[],\"cursor\":\"5\"}",
      "{\"events\":[],\"cursor\":\"5\"}",
      "[]",
  };
  ASSERT_EQ(d.appendRaw("uno"), Outbox::AppendResult::Ok);
  for (const std::string& body : bodies) {
    d.http.log.clear();
    d.http.script.push_back(jsonReply(200, body));
    const SessionReport report = manual();
    EXPECT_EQ(report.outcome, SessionOutcome::ProtocolError) << body;
    EXPECT_TRUE(report.pullFailed) << body;
    EXPECT_TRUE(report.drainBlockedByPull) << body;
    EXPECT_EQ(d.store->state().cursor, 0u) << body;
    EXPECT_EQ(d.http.countPath("/api/sync/v2/exchange"), 0u) << body;
    EXPECT_EQ(d.outbox->depth(), 1u) << body;
    EXPECT_STREQ(d.store->state().diag.code, "malformed_response") << body;
  }
}

TEST_F(SyncEngineTest, MalformedPageAppliesNoneOfItsEvents) {
  const std::string good = remoteProgress(10, kOtherId, kMd5B, 33);
  d.http.script.push_back(jsonReply(200, "{\"protocol_version\":2,\"events\":[" + good + ",{\"a\":tru}],\"cursor\":\"10\"}"));
  EXPECT_EQ(manual().outcome, SessionOutcome::ProtocolError);
  RemoteHead head;
  EXPECT_FALSE(d.engine->remoteHead(Device::book(kMd5B), head));
}

TEST_F(SyncEngineTest, OversizedEventIsSkippedAndTheCursorAdvances) {
  d.server.delivered = 1u << 30;  // These replies are scripted, past the model server.
  const std::string before = remoteProgress(10, kOtherId, kMd5B, 11);
  const std::string after = remoteProgress(12, kOtherId, kMd5B, 12);
  std::string poison = remoteProgress(11, kOtherId, kMd5B, 99);
  poison.insert(poison.size() - 1, ",\"padding\":\"" + std::string(40000, 'p') + "\"");
  d.http.script.push_back(jsonReply(200, "{\"protocol_version\":2,\"events\":[" + before + "," + poison + "," + after +
                                             "],\"cursor\":\"12\",\"high_watermark\":\"12\",\"has_more\":false}"));
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::Ok);
  EXPECT_EQ(report.eventsSkipped, 1);
  EXPECT_EQ(report.eventsApplied, 2);
  EXPECT_EQ(d.store->state().cursor, 12u);
  EXPECT_EQ(d.store->state().skippedEvents, 1u);
  EXPECT_EQ(d.store->state().lastSkippedSeq, 11u);
  RemoteHead head;
  ASSERT_TRUE(d.engine->remoteHead(Device::book(kMd5B), head));
  EXPECT_EQ(head.position.pctMicro, 12000000u);
}

TEST_F(SyncEngineTest, DeepNestingInAnEventIsNotAPoisonPage) {
  d.server.delivered = 1u << 30;  // These replies are scripted, past the model server.
  std::string poison = remoteProgress(11, kOtherId, kMd5B, 99);
  poison.insert(poison.size() - 1, ",\"deep\":" + std::string(100000, '[') + std::string(100000, ']'));
  d.http.script.push_back(jsonReply(200, "{\"protocol_version\":2,\"events\":[" + poison + "],\"cursor\":\"11\",\"has_more\":false}"));
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::Ok);
  EXPECT_EQ(report.eventsSkipped, 1);
  EXPECT_EQ(d.store->state().cursor, 11u);
}

TEST_F(SyncEngineTest, HostileEventsAdvanceTheCursorAndLeaveNothingBehind) {
  d.server.delivered = 1u << 30;  // These replies are scripted, past the model server.
  const std::string events =
      "[5,\"x\",null,[],{},"
      "{\"server_sequence\":\"10\",\"event_type\":\"progress.changed\",\"book_identifier\":{\"kind\":\"koreader_partial_md5\",\"value\":\"../../etc/passwd\"}},"
      "{\"server_sequence\":\"11\",\"event_type\":\"progress.changed\",\"book_identifier\":{\"kind\":\"sha256\",\"value\":\"" + std::string(kMd5A) + "\"}},"
      "{\"server_sequence\":\"12\",\"event_type\":\"made.up\",\"book_identifier\":{\"kind\":\"koreader_partial_md5\",\"value\":\"" + std::string(kMd5A) + "\"}},"
      "{\"server_sequence\":\"not-a-number\",\"event_type\":\"progress.changed\"}]";
  d.http.script.push_back(jsonReply(200, "{\"protocol_version\":2,\"events\":" + events + ",\"cursor\":\"12\",\"has_more\":false}"));
  const SessionReport report = manual();
  EXPECT_EQ(report.outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.store->state().cursor, 12u);
  for (const std::string& path : d.fs.paths()) EXPECT_EQ(path.find("passwd"), std::string::npos) << path;
  RemoteHead head;
  EXPECT_FALSE(d.engine->remoteHead(Device::book(), head));
}

TEST_F(SyncEngineTest, ReplayedProgressNeverPrompts) {
  d.server.addFeedEvent(10, remoteProgress(10, kOtherId, kMd5A, 80, "/body/p[8].0", "", "suggest_resume"));
  ResumeDecision decision;
  // A book with no focus event, even though a pull will carry its progress.
  const SessionReport report = open(decision);
  EXPECT_EQ(report.outcome, SessionOutcome::Ok);
  EXPECT_FALSE(decision.ask);
  RemoteHead head;
  ASSERT_TRUE(d.engine->remoteHead(Device::book(), head));
  EXPECT_EQ(head.position.pctMicro, 80000000u);
  EXPECT_STREQ(head.deviceName, "Kobo");
  EXPECT_EQ(head.precision, TimePrecision::Exact);
}

TEST_F(SyncEngineTest, OwnReplayedEventsDoNotTouchTheHeadCache) {
  d.server.addFeedEvent(10, remoteProgress(10, kSelfId, kMd5A, 80));
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  RemoteHead head;
  EXPECT_FALSE(d.engine->remoteHead(Device::book(), head));
}

TEST_F(SyncEngineTest, HeadCacheKeepsTheNewestWhenPagesReplay) {
  d.http.script.push_back(jsonReply(200, "{\"protocol_version\":2,\"events\":[" + remoteProgress(20, kOtherId, kMd5B, 20) + "," +
                                             remoteProgress(10, kOtherId, kMd5B, 10) + "],\"cursor\":\"20\",\"has_more\":false}"));
  manual();
  RemoteHead head;
  ASSERT_TRUE(d.engine->remoteHead(Device::book(kMd5B), head));
  EXPECT_EQ(head.serverSeq, 20u);
  EXPECT_EQ(head.position.pctMicro, 20000000u);
}

// --- Inbound annotations ----------------------------------------------------------

namespace {
std::string remoteAnnotation(uint64_t seq, const std::string& origin, const std::string& type, const std::string& text) {
  return "{\"event_id\":\"evt-" + std::to_string(seq) + "\",\"server_sequence\":\"" + std::to_string(seq) +
         "\",\"origin_device\":{\"id\":\"" + origin + "\",\"name\":\"Kobo\"},\"event_type\":\"" + type +
         "\",\"book_identifier\":{\"kind\":\"koreader_partial_md5\",\"value\":\"" + std::string(kMd5A) +
         "\"},\"payload\":{\"sync_id\":\"50000000-0000-4000-8000-000000000001\",\"text\":\"" + text +
         "\",\"chapter\":\"Uno\",\"note\":\"nota\",\"page\":7},\"directive\":\"apply_remote\",\"directive_metadata\":{}}";
}

}  // namespace

TEST_F(SyncEngineTest, InboundAnnotationsMoveTheCursorAndLeaveNothingOnTheCard) {
  d.server.addFeedEvent(10, remoteAnnotation(10, kOtherId, "annotation.created", std::string(3000, 'a')));
  d.server.addFeedEvent(11, remoteAnnotation(11, kOtherId, "annotation.deleted", ""));
  d.server.addFeedEvent(12, remoteAnnotation(12, kSelfId, "annotation.created", "propio"));
  EXPECT_EQ(manual().eventsApplied, 2);
  EXPECT_EQ(d.store->state().cursor, 12u);
  for (const std::string& path : d.fs.paths()) {
    if (path != kSpoolPath) EXPECT_EQ(d.fs.content(path).find("aaaaaaaa"), std::string::npos) << path;
  }
}

// --- Focus check ------------------------------------------------------------------

TEST_F(SyncEngineTest, FocusNullReleasesTheHoldAndDrains) {
  ASSERT_EQ(d.appendRaw("p", nullptr, kMd5A, 0, EventKind::Progress), Outbox::AppendResult::Ok);
  d.server.focusEvent = "null";
  ResumeDecision decision;
  const SessionReport report = open(decision);
  EXPECT_EQ(report.outcome, SessionOutcome::Ok);
  EXPECT_TRUE(report.focusChecked);
  EXPECT_FALSE(decision.ask);
  EXPECT_EQ(decision.verdict, resume::Verdict::NoEvent);
  EXPECT_EQ(d.store->state().holds.size(), 0);
  EXPECT_TRUE(d.server.hasText("p"));
}

TEST_F(SyncEngineTest, FocusOwnReleasesTheHold) {
  d.server.focusEvent = focusEventJson(kSelfId, kMd5A, 50);
  ResumeDecision decision;
  EXPECT_EQ(open(decision).outcome, SessionOutcome::Ok);
  EXPECT_FALSE(decision.ask);
  EXPECT_EQ(decision.verdict, resume::Verdict::Own);
  EXPECT_EQ(d.store->state().holds.size(), 0);
}

TEST_F(SyncEngineTest, FocusOtherAsksAndHoldsBackProgress) {
  ASSERT_EQ(d.appendRaw("mine", nullptr, kMd5A, 0, EventKind::Progress), Outbox::AppendResult::Ok);
  d.server.focusEvent = focusEventJson(kOtherId, kMd5A, 60);
  ResumeDecision decision;
  const SessionReport report = open(decision);
  EXPECT_EQ(report.outcome, SessionOutcome::AwaitingChoice);
  EXPECT_TRUE(decision.ask);
  EXPECT_STREQ(decision.remoteDeviceName, "Kobo Clara");
  EXPECT_EQ(decision.remotePct, 60000000u);
  EXPECT_TRUE(decision.xpointerUsable);
  EXPECT_STREQ(decision.remoteXpointer, "/body/p[9].0");
  EXPECT_TRUE(decision.showDate);
  EXPECT_STREQ(decision.occurredAt, "2026-11-05T10:00:00.000Z");
  EXPECT_EQ(decision.warnings, 0);
  // The prompt does not wait on history, and nothing went up.
  EXPECT_EQ(d.http.log.size(), 1u);
  EXPECT_EQ(report.held, 1u);
  EXPECT_FALSE(d.server.hasText("mine"));
  // The connection stays for applyChoice().
  EXPECT_EQ(d.http.disconnects, 0);
}

TEST_F(SyncEngineTest, FocusRequestIsReadOnlyAndExact) {
  d.store->state().cursor = 7;
  d.server.delivered = 7;
  ResumeDecision decision;
  open(decision);
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, d.http.log[0].body));
  EXPECT_EQ(doc["protocol_version"], 2);
  EXPECT_EQ(doc["cursor"], "7");
  EXPECT_EQ(doc["focus_only"], true);
  EXPECT_EQ(doc["focus_book"]["kind"], "koreader_partial_md5");
  EXPECT_EQ(doc["focus_book"]["value"], kMd5A);
}

TEST_F(SyncEngineTest, UnknownClockEventShowsNoDate) {
  d.server.focusEvent = focusEventJson(kOtherId, kMd5A, 60, "evt-500", "/body/p[9].0", "unknown");
  ResumeDecision decision;
  open(decision);
  EXPECT_TRUE(decision.ask);
  EXPECT_FALSE(decision.showDate);
  EXPECT_EQ(decision.occurredAt[0], '\0');
}

TEST_F(SyncEngineTest, WarningsReachTheDecision) {
  d.server.focusEvent = focusEventJson(kOtherId, kMd5A, 10, "evt-500", "");
  ResumeDecision decision;
  open(decision, 50);
  EXPECT_EQ(decision.warnings, resume::kWarnBackwards | resume::kWarnApproximate);
  EXPECT_FALSE(decision.xpointerUsable);
}

TEST_F(SyncEngineTest, InvalidFocusIsAReportedErrorWithNoPrompt) {
  const std::vector<std::string> invalid = {
      focusEventJson(kOtherId, kMd5B, 50),  // Wrong book.
      "{\"server_sequence\":\"5\",\"origin_device\":{\"id\":\"x\"},\"book_identifier\":{\"kind\":\"koreader_partial_md5\",\"value\":\"" +
          std::string(kMd5A) + "\"},\"payload\":{\"percentage\":5},\"directive_metadata\":{\"reading_head\":true}}",  // No event_id.
      "{\"event_id\":\"e\",\"origin_device\":{\"id\":\"x\"},\"book_identifier\":{\"kind\":\"koreader_partial_md5\",\"value\":\"" +
          std::string(kMd5A) + "\"},\"payload\":{\"percentage\":5},\"directive_metadata\":{}}",  // No reading_head.
      "{\"event_id\":\"e\",\"book_identifier\":{\"kind\":\"koreader_partial_md5\",\"value\":\"" + std::string(kMd5A) +
          "\"},\"payload\":{\"percentage\":5},\"directive_metadata\":{\"reading_head\":true}}",  // No origin.
      "{}",
  };
  for (const std::string& focus : invalid) {
    d.http.log.clear();
    d.server.focusEvent = focus;
    d.store->state().holds.clear();
    ResumeDecision decision;
    const SessionReport report = open(decision);
    EXPECT_TRUE(report.focusInvalid) << focus;
    EXPECT_FALSE(decision.ask) << focus;
    EXPECT_EQ(decision.verdict, resume::Verdict::Invalid) << focus;
    EXPECT_STREQ(d.store->state().diag.code, "reading_head_invalid") << focus;
    // The hold stays: nothing stale goes up on an answer nobody can read.
    EXPECT_TRUE(d.store->state().holds.contains(Device::book().md5)) << focus;
    // The rest of the session still runs.
    EXPECT_EQ(report.outcome, SessionOutcome::Ok) << focus;
    EXPECT_GE(d.http.countPath("/api/sync/v2/pull"), 2u) << focus;
  }
}

TEST_F(SyncEngineTest, ServerThatIgnoresFocusOnlyLosesNothing) {
  d.server.ignoreFocusOnly = true;
  d.server.addFeedEvent(10, remoteProgress(10, kOtherId, kMd5B, 33));
  ResumeDecision decision;
  const SessionReport report = open(decision);
  EXPECT_TRUE(report.focusInvalid);
  EXPECT_FALSE(decision.ask);
  EXPECT_EQ(d.store->state().cursor, 10u);
  EXPECT_TRUE(d.store->state().holds.contains(Device::book().md5));
  RemoteHead head;
  EXPECT_TRUE(d.engine->remoteHead(Device::book(kMd5B), head));
}

TEST_F(SyncEngineTest, FocusFailureStopsTheSessionAndKeepsTheHold) {
  ASSERT_EQ(d.appendRaw("mine", nullptr, kMd5A, 0, EventKind::Progress), Outbox::AppendResult::Ok);
  d.http.script.push_back(apiError(503, "sync_service_unavailable"));
  ResumeDecision decision;
  const SessionReport report = open(decision);
  EXPECT_EQ(report.outcome, SessionOutcome::ServerError);
  EXPECT_FALSE(decision.ask);
  EXPECT_TRUE(d.store->state().holds.contains(Device::book().md5));
  EXPECT_EQ(d.http.countPath("/api/sync/v2/exchange"), 0u);
  EXPECT_EQ(d.http.disconnects, 1);
}

// --- Choices ----------------------------------------------------------------------

TEST_F(SyncEngineTest, GoThereJumpsSavesUndoAndBecomesTheHead) {
  d.server.focusEvent = focusEventJson(kOtherId, kMd5A, 60);
  ResumeDecision decision;
  ASSERT_EQ(open(decision).outcome, SessionOutcome::AwaitingChoice);

  const Position before = Device::position(20, "/body/p[2].0");
  const Position after = Device::position(60, "/body/p[9].0");
  const SessionReport report = d.engine->applyChoice(decision, ResumeChoice::GoThere, before, after);
  EXPECT_EQ(report.outcome, SessionOutcome::Ok);
  EXPECT_EQ(report.depth, 0u);
  EXPECT_EQ(d.store->state().holds.size(), 0);

  ASSERT_EQ(d.server.recorded.size(), 1u);
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, d.server.recorded[0].body));
  EXPECT_EQ(doc["event_type"], "progress.changed");
  EXPECT_EQ(doc["payload"]["percentage"], 60);
  EXPECT_EQ(doc["payload"]["explicit_jump"], true);
  EXPECT_EQ(doc["payload"]["xpointer"], "/body/p[9].0");

  // Undo is there, once.
  EXPECT_TRUE(d.engine->hasUndo(Device::book()));
  Position undo;
  ASSERT_TRUE(d.engine->takeUndo(Device::book(), undo));
  EXPECT_STREQ(undo.xpointer, "/body/p[2].0");
  EXPECT_EQ(undo.pctMicro, 20000000u);
  EXPECT_FALSE(d.engine->hasUndo(Device::book()));
  EXPECT_FALSE(d.engine->takeUndo(Device::book(), undo));
  EXPECT_EQ(d.http.disconnects, 1);
}

TEST_F(SyncEngineTest, StayHereBecomesTheHeadWithoutJumping) {
  d.server.focusEvent = focusEventJson(kOtherId, kMd5A, 60);
  ResumeDecision decision;
  ASSERT_EQ(open(decision).outcome, SessionOutcome::AwaitingChoice);
  const Position here = Device::position(20, "/body/p[2].0");
  const SessionReport report = d.engine->applyChoice(decision, ResumeChoice::StayHere, here, here);
  EXPECT_EQ(report.outcome, SessionOutcome::Ok);
  ASSERT_EQ(d.server.recorded.size(), 1u);
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, d.server.recorded[0].body));
  EXPECT_EQ(doc["payload"]["percentage"], 20);
  EXPECT_EQ(doc["payload"]["explicit_jump"], false);
  char now[21];
  formatRfc3339(kNow, now);
  EXPECT_EQ(doc["occurred_at"], now) << "stamped now, not at the last page turn";
  EXPECT_FALSE(d.engine->hasUndo(Device::book()));
}

TEST_F(SyncEngineTest, StayWithNoLocalPositionPublishesNothingAndStillAnswers) {
  d.server.focusEvent = focusEventJson(kOtherId, kMd5A, 60);
  ResumeDecision decision;
  Position nowhere;
  ASSERT_EQ(d.engine->runOpenSession(Device::bookRef(), nowhere, decision).outcome, SessionOutcome::AwaitingChoice);
  EXPECT_EQ(d.engine->applyChoice(decision, ResumeChoice::StayHere, nowhere, nowhere).outcome, SessionOutcome::Ok);
  EXPECT_TRUE(d.server.recorded.empty()) << "no percentage:0 to become the head";
  EXPECT_EQ(d.outbox->depth(), 0u);
  EXPECT_EQ(d.store->state().holds.size(), 0);
  ResumeDecision again;
  EXPECT_EQ(d.engine->runOpenSession(Device::bookRef(), nowhere, again).outcome, SessionOutcome::Ok);
  EXPECT_FALSE(again.ask);
}

TEST_F(SyncEngineTest, FinalizeWithNoPercentagePublishesNoProgress) {
  FinalizeInput input = d.finalizeInput(30);
  input.position.hasPct = false;
  input.position.pctMicro = 0;
  const FinalizeResult result = d.engine->finalizeBook(input);
  EXPECT_TRUE(result.ok);
  EXPECT_FALSE(result.progressQueued);
  EXPECT_EQ(d.outbox->depth(), 0u);
}

TEST_F(SyncEngineTest, ReadingAfterAnAnswerDoesNotReopenTheQuestion) {
  // Go, read offline, reopen online: a second prompt would let Go throw away those pages.
  d.server.focusEvent = focusEventJson(kOtherId, kMd5A, 60);
  ResumeDecision decision;
  ASSERT_EQ(open(decision).outcome, SessionOutcome::AwaitingChoice);
  const Position here = Device::position(20);
  const Position there = Device::position(60);
  ASSERT_EQ(d.engine->applyChoice(decision, ResumeChoice::GoThere, here, there).outcome, SessionOutcome::Ok);
  FinalizeInput input = d.finalizeInput(75, "/body/p[20].0", kNow + 600);
  ASSERT_TRUE(d.engine->finalizeBook(input).progressQueued);

  ResumeDecision again;
  EXPECT_EQ(d.engine->runOpenSession(Device::bookRef(), Device::position(75), again).outcome, SessionOutcome::Ok);
  EXPECT_FALSE(again.ask);
  EXPECT_EQ(again.verdict, resume::Verdict::AlreadyChosen);
}

TEST_F(SyncEngineTest, AnsweredEventDoesNotAskAgain) {
  d.server.focusEvent = focusEventJson(kOtherId, kMd5A, 60);
  ResumeDecision decision;
  ASSERT_EQ(open(decision).outcome, SessionOutcome::AwaitingChoice);
  const Position here = Device::position(20);
  d.engine->applyChoice(decision, ResumeChoice::StayHere, here, here, false);

  // Same event comes back, offline upload notwithstanding: no prompt, hold released.
  ResumeDecision again;
  EXPECT_EQ(open(again).outcome, SessionOutcome::Ok);
  EXPECT_FALSE(again.ask);
  EXPECT_EQ(again.verdict, resume::Verdict::AlreadyChosen);

  // Survives a reboot.
  d.crashAndBoot();
  ResumeDecision rebooted;
  EXPECT_EQ(open(rebooted).outcome, SessionOutcome::Ok);
  EXPECT_FALSE(rebooted.ask);
}

TEST_F(SyncEngineTest, NewEventAtTheSameSpotAsksAgain) {
  d.server.focusEvent = focusEventJson(kOtherId, kMd5A, 60, "evt-1");
  ResumeDecision decision;
  ASSERT_EQ(open(decision).outcome, SessionOutcome::AwaitingChoice);
  const Position here = Device::position(20);
  d.engine->applyChoice(decision, ResumeChoice::StayHere, here, here, false);
  d.server.focusEvent = focusEventJson(kOtherId, kMd5A, 60, "evt-2");
  ResumeDecision again;
  EXPECT_EQ(open(again).outcome, SessionOutcome::AwaitingChoice);
}

TEST_F(SyncEngineTest, AbandoningThePromptKeepsTheHoldAndAsksNextTime) {
  ASSERT_EQ(d.appendRaw("mine", nullptr, kMd5A, 0, EventKind::Progress), Outbox::AppendResult::Ok);
  d.server.focusEvent = focusEventJson(kOtherId, kMd5A, 60);
  ResumeDecision decision;
  ASSERT_EQ(open(decision).outcome, SessionOutcome::AwaitingChoice);
  const SessionReport report = d.engine->abandonChoice();
  EXPECT_EQ(report.held, 1u);
  EXPECT_EQ(d.http.disconnects, 1);
  EXPECT_FALSE(d.server.hasText("mine"));
  ResumeDecision again;
  EXPECT_EQ(open(again).outcome, SessionOutcome::AwaitingChoice);
}

TEST_F(SyncEngineTest, ApplyChoiceWithoutAPendingDecisionIsRefused) {
  ResumeDecision made_up;
  made_up.book = Device::book();
  made_up.fingerprint = 5;
  const Position here = Device::position(20);
  EXPECT_EQ(d.engine->applyChoice(made_up, ResumeChoice::GoThere, here, here).outcome, SessionOutcome::StaleChoice);
  EXPECT_EQ(d.outbox->depth(), 0u);
}

TEST_F(SyncEngineTest, OfflineChoiceIsDurableAndGoesOutNextSession) {
  d.server.focusEvent = focusEventJson(kOtherId, kMd5A, 60);
  ResumeDecision decision;
  ASSERT_EQ(open(decision).outcome, SessionOutcome::AwaitingChoice);
  const Position here = Device::position(20);
  const SessionReport report = d.engine->applyChoice(decision, ResumeChoice::StayHere, here, here, false);
  EXPECT_EQ(report.depth, 1u);
  EXPECT_EQ(report.held, 0u);
  d.crashAndBoot();
  EXPECT_EQ(d.outbox->depth(), 1u);
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.server.recorded.size(), 1u);
}

TEST_F(SyncEngineTest, PendingSuggestionIsResolvedWithTheChoice) {
  const std::string suggestion = "40000000-0000-4000-8000-000000000001";
  // A replayed pull brought an open suggestion for this book.
  d.server.addFeedEvent(10, remoteProgress(10, kOtherId, kMd5A, 60, "", "{\"suggestion\":{\"id\":\"" + suggestion + "\"}}"));
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);

  d.server.focusEvent = focusEventJson(kOtherId, kMd5A, 60);
  ResumeDecision decision;
  ASSERT_EQ(open(decision).outcome, SessionOutcome::AwaitingChoice);
  d.http.log.clear();
  const Position here = Device::position(20);
  d.engine->applyChoice(decision, ResumeChoice::GoThere, here, here);
  EXPECT_EQ(d.http.countPath("/api/sync/v2/suggestions/" + suggestion + "/accept"), 1u);
  BookLedger ledger;
  ASSERT_TRUE(d.store->loadBook(Device::book(), ledger));
  EXPECT_EQ(ledger.suggestionId[0], '\0');
}

TEST_F(SyncEngineTest, SuggestionSurvivesAFailedResolveAndIsDismissedOnStay) {
  const std::string suggestion = "40000000-0000-4000-8000-000000000002";
  d.server.addFeedEvent(10, remoteProgress(10, kOtherId, kMd5A, 60, "", "{\"suggestion\":{\"id\":\"" + suggestion + "\"}}"));
  ASSERT_EQ(manual().outcome, SessionOutcome::Ok);
  d.server.focusEvent = focusEventJson(kOtherId, kMd5A, 60);
  ResumeDecision decision;
  ASSERT_EQ(open(decision).outcome, SessionOutcome::AwaitingChoice);
  d.http.log.clear();
  d.http.handler = [this, suggestion](const RecordedRequest& r) {
    if (r.path.find("/suggestions/") != std::string::npos) return apiError(503, "sync_service_unavailable");
    return d.server.handle(r);
  };
  const Position here = Device::position(20);
  d.engine->applyChoice(decision, ResumeChoice::StayHere, here, here);
  EXPECT_EQ(d.http.countPath("/api/sync/v2/suggestions/" + suggestion + "/dismiss"), 1u);
  BookLedger ledger;
  ASSERT_TRUE(d.store->loadBook(Device::book(), ledger));
  EXPECT_STREQ(ledger.suggestionId, suggestion.c_str());
  EXPECT_EQ(ledger.suggestionAction, SuggestionAction::Dismiss);
}

// --- Sleep and manual -------------------------------------------------------------

TEST_F(SyncEngineTest, SleepGateSkipsTheRadioWhenThereIsNothingToSend) {
  EXPECT_EQ(d.engine->gate(SessionKind::Sleep), Gate::NothingToDo);
  ASSERT_EQ(d.appendRaw("uno"), Outbox::AppendResult::Ok);
  EXPECT_EQ(d.engine->gate(SessionKind::Sleep), Gate::Go);
}

TEST_F(SyncEngineTest, SleepSessionResolvesAHeldBookWithNoPrompt) {
  ASSERT_TRUE(d.engine->onBookOpened(Device::book()));
  ASSERT_EQ(d.appendRaw("mine", nullptr, kMd5A, 0, EventKind::Progress), Outbox::AppendResult::Ok);
  d.server.focusEvent = "null";
  const SessionReport report = d.engine->runSleepSession();
  EXPECT_EQ(report.outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.store->state().holds.size(), 0);
  EXPECT_TRUE(d.server.hasText("mine"));
}

TEST_F(SyncEngineTest, SleepSessionNeverPromptsAndKeepsTheHoldIfAChoiceIsNeeded) {
  ASSERT_TRUE(d.engine->onBookOpened(Device::book()));
  std::string other;
  ASSERT_EQ(d.appendRaw("mine", nullptr, kMd5A, 0, EventKind::Progress), Outbox::AppendResult::Ok);
  ASSERT_EQ(d.appendRaw("note", &other), Outbox::AppendResult::Ok);
  d.server.focusEvent = focusEventJson(kOtherId, kMd5A, 60);
  const SessionReport report = d.engine->runSleepSession();
  EXPECT_EQ(report.outcome, SessionOutcome::Ok);
  EXPECT_EQ(report.held, 1u);
  EXPECT_FALSE(d.server.hasText("mine"));
  EXPECT_TRUE(d.server.hasText("note"));
  EXPECT_TRUE(d.store->state().holds.contains(Device::book().md5));
}

TEST_F(SyncEngineTest, HeldBookWithNothingWaitingCostsNoRequest) {
  ASSERT_TRUE(d.engine->onBookOpened(Device::book()));
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  for (const auto& r : d.http.log) EXPECT_EQ(r.body.find("focus_only"), std::string::npos);
}

TEST_F(SyncEngineTest, ManualFromTheDrawerCanPrompt) {
  d.server.focusEvent = focusEventJson(kOtherId, kMd5A, 60);
  const BookRef book = Device::bookRef();
  const Position here = Device::position(20);
  ResumeDecision decision;
  const SessionReport report = d.engine->runManualSession(&book, &here, &decision);
  EXPECT_EQ(report.outcome, SessionOutcome::AwaitingChoice);
  EXPECT_TRUE(decision.ask);
  EXPECT_EQ(d.engine->applyChoice(decision, ResumeChoice::StayHere, here, here).outcome, SessionOutcome::Ok);
}

TEST_F(SyncEngineTest, NotPairedDoesNothing) {
  d.store->forgetCredential();
  EXPECT_EQ(d.engine->gate(SessionKind::Manual), Gate::NotPaired);
  EXPECT_EQ(manual().outcome, SessionOutcome::NotPaired);
  EXPECT_TRUE(d.http.log.empty());
}

// --- Capabilities -----------------------------------------------------------------

TEST_F(SyncEngineTest, CapabilitiesAreCheckedOnceBeforeTheFirstUpload) {
  ASSERT_EQ(d.appendRaw("a"), Outbox::AppendResult::Ok);
  manual();
  EXPECT_EQ(d.http.countPath("/api/sync/v2/capabilities"), 1u);
  ASSERT_EQ(d.appendRaw("b"), Outbox::AppendResult::Ok);
  d.crashAndBoot();
  manual();
  EXPECT_EQ(d.http.countPath("/api/sync/v2/capabilities"), 1u);
}

TEST_F(SyncEngineTest, UnsupportedCapabilitiesKeepTheQueueClosed) {
  ASSERT_EQ(d.appendRaw("a"), Outbox::AppendResult::Ok);
  const std::vector<std::string> bodies = {
      "{\"protocol_version\":3,\"event_versions\":{}}",
      "{\"protocol_version\":2,\"event_versions\":{\"progress.changed\":[2]}}",
      "{\"protocol_version\":2}",
      "not json",
  };
  for (const std::string& body : bodies) {
    d.http.log.clear();
    d.http.handler = [&](const RecordedRequest& r) {
      if (r.path == "/api/sync/v2/capabilities") return jsonReply(200, body);
      return d.server.handle(r);
    };
    const SessionReport report = manual();
    EXPECT_EQ(report.outcome, SessionOutcome::ProtocolError) << body;
    EXPECT_STREQ(d.store->state().diag.code, "capabilities_unsupported") << body;
    EXPECT_EQ(d.http.countPath("/api/sync/v2/exchange"), 0u) << body;
    EXPECT_EQ(d.outbox->depth(), 1u) << body;
  }
}

// --- Credential rotation and health -----------------------------------------------

TEST_F(SyncEngineTest, RotatesAfterRenewAfterAndKeepsTheNewToken) {
  d.store->creds().renewAfterEpoch = kNow - 10;
  d.http.handler = [this](const RecordedRequest& r) {
    if (r.path == "/api/devices/self/credentials/rotate") {
      return jsonReply(200, "{\"credential\":{\"token\":\"toto_NEW\",\"expires_at\":\"2027-06-01T00:00:00.000Z\"},\"grace_seconds\":300}");
    }
    return d.server.handle(r);
  };
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_STREQ(d.store->creds().token, "toto_NEW");
  EXPECT_EQ(d.store->creds().renewAfterEpoch, 0);
  EXPECT_GT(d.store->creds().expiresAtEpoch, kNow);
  d.crashAndBoot();
  EXPECT_STREQ(d.store->creds().token, "toto_NEW");
}

TEST_F(SyncEngineTest, NoRotationWithoutADateOrWithAnUntrustedClock) {
  manual();
  EXPECT_EQ(d.http.countPath("/api/devices/self/credentials/rotate"), 0u);
  d.store->creds().renewAfterEpoch = kNow - 10;
  d.clock.wall.precision = TimePrecision::Unknown;
  d.clock.setCalls = 0;
  d.http.handler = [this](const RecordedRequest& r) {
    Reply reply = d.server.handle(r);
    // Keep the clock unknown even though server_time would fix it.
    size_t at = reply.body.find("server_time");
    if (at != std::string::npos) reply.body.replace(at, 11, "server_timx");
    return reply;
  };
  manual();
  EXPECT_EQ(d.http.countPath("/api/devices/self/credentials/rotate"), 0u);
}

TEST_F(SyncEngineTest, FailedRotationKeepsTheOldToken) {
  d.store->creds().renewAfterEpoch = kNow - 10;
  d.http.handler = [this](const RecordedRequest& r) {
    if (r.path == "/api/devices/self/credentials/rotate") return apiError(503, "x");
    return d.server.handle(r);
  };
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_STREQ(d.store->creds().token, "toto_SECRET_TOKEN_VALUE");
}

TEST_F(SyncEngineTest, HealthReportsSuccessAndErrorsInTheServersShape) {
  ASSERT_EQ(d.appendRaw("a"), Outbox::AppendResult::Ok);
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  JsonDocument ok;
  for (const auto& r : d.http.log) {
    if (r.path == "/api/devices/self/health") ASSERT_FALSE(deserializeJson(ok, r.body));
  }
  EXPECT_EQ(ok["outcome"], "success");
  EXPECT_EQ(ok["queue_depth"], 0);
  EXPECT_TRUE(ok["error_code"].isUnbound());  // The server refuses error fields on success.

  d.http.log.clear();
  d.http.handler = [this](const RecordedRequest& r) {
    if (r.path == "/api/sync/v2/pull") return apiError(500, "boom");
    return d.server.handle(r);
  };
  manual();
  JsonDocument bad;
  for (const auto& r : d.http.log) {
    if (r.path == "/api/devices/self/health") ASSERT_FALSE(deserializeJson(bad, r.body));
  }
  EXPECT_EQ(bad["outcome"], "error");
  EXPECT_EQ(bad["error_code"], "boom");
}

TEST_F(SyncEngineTest, OpenSessionsSpendNoBudgetOnHealth) {
  ResumeDecision decision;
  open(decision);
  EXPECT_EQ(d.http.countPath("/api/devices/self/health"), 0u);
}

// --- Cancel and budget ------------------------------------------------------------

TEST_F(SyncEngineTest, CancelAtEachStepLosesNothing) {
  // How many requests does the complete session make?
  size_t total = 0;
  {
    Device probe;
    probe.boot();
    for (int i = 0; i < 25; ++i) ASSERT_EQ(probe.appendRaw("e" + std::to_string(i)), Outbox::AppendResult::Ok);
    probe.server.addFeedEvent(10, remoteProgress(10, kOtherId, kMd5B, 33));
    probe.server.focusEvent = "null";
    ResumeDecision decision;
    ASSERT_EQ(probe.engine->runOpenSession(Device::bookRef(), Device::position(1), decision).outcome, SessionOutcome::Ok);
    total = probe.http.log.size();
  }
  ASSERT_GE(total, 4u);

  for (size_t cancelAt = 0; cancelAt <= total; ++cancelAt) {
    Device dev;
    dev.boot();
    std::vector<std::string> ids(25);
    for (int i = 0; i < 25; ++i) ASSERT_EQ(dev.appendRaw("e" + std::to_string(i), &ids[static_cast<size_t>(i)]), Outbox::AppendResult::Ok);
    dev.server.addFeedEvent(10, remoteProgress(10, kOtherId, kMd5B, 33));
    dev.server.focusEvent = "null";
    size_t countAtCancel = 0;
    dev.http.beforeRequest = [&](size_t index) {
      if (index == cancelAt) {
        dev.cancel.flag = true;
        countAtCancel = index;
      }
    };
    ResumeDecision decision;
    const SessionReport report = dev.engine->runOpenSession(Device::bookRef(), Device::position(1), decision);
    if (cancelAt < total) {
      // The last request is the cursor commit, which is best effort: all the work is
      // already done, and the next request carries that cursor anyway.
      if (cancelAt + 1 < total) EXPECT_EQ(report.outcome, SessionOutcome::Cancelled) << "cancelAt=" << cancelAt;
      // Nothing after the cancel: the fake sees that one request abort, and no more.
      EXPECT_EQ(dev.http.log.size(), countAtCancel + 1) << "cancelAt=" << cancelAt;
    }
    EXPECT_GE(dev.http.disconnects, 1) << "cancelAt=" << cancelAt;

    // Next session finishes everything, with the same ids.
    dev.http.beforeRequest = nullptr;
    dev.cancel.flag = false;
    EXPECT_EQ(dev.engine->runManualSession(nullptr, nullptr, nullptr).outcome, SessionOutcome::Ok) << "cancelAt=" << cancelAt;
    EXPECT_EQ(dev.outbox->depth(), 0u);
    for (const std::string& id : ids) EXPECT_EQ(dev.server.byId.count(id), 1u) << "cancelAt=" << cancelAt;
    EXPECT_EQ(dev.server.recorded.size(), ids.size());
    EXPECT_EQ(dev.server.violations, std::vector<std::string>{});
  }
}

TEST_F(SyncEngineTest, CancelInsideAResponseStream) {
  ASSERT_EQ(d.appendRaw("a"), Outbox::AppendResult::Ok);
  std::string events;
  for (uint64_t seq = 10; seq < 20; ++seq) {
    if (!events.empty()) events += ",";
    events += remoteProgress(seq, kOtherId, kMd5B, 1);
  }
  d.http.chunk = 64;
  size_t polls = 0;
  d.http.handler = [&](const RecordedRequest&) {
    return jsonReply(200, "{\"protocol_version\":2,\"events\":[" + events + "],\"cursor\":\"19\",\"has_more\":false}");
  };
  // Trips the cancel flag partway through by counting clock reads the abort check does.
  struct FlipAfter : public FakeCancel {
    size_t* counter;
    size_t limit;
    bool cancelled() override { return ++*counter > limit; }
  };
  FlipAfter flip;
  flip.counter = &polls;
  flip.limit = 12;
  auto engine = std::make_unique<SyncEngine>(*d.store, *d.outbox, d.fs, d.http, d.clock, d.random, flip, d.config);
  ASSERT_TRUE(engine->begin());
  const SessionReport report = engine->runManualSession(nullptr, nullptr, nullptr);
  EXPECT_EQ(report.outcome, SessionOutcome::Cancelled);
  EXPECT_EQ(d.store->state().cursor, 0u);
  EXPECT_EQ(d.outbox->depth(), 1u);
  EXPECT_EQ(d.http.countPath("/api/sync/v2/exchange"), 0u);
}

TEST_F(SyncEngineTest, BudgetExhaustionStopsBetweenStepsAndLosesNothing) {
  for (int i = 0; i < 25; ++i) ASSERT_EQ(d.appendRaw("e" + std::to_string(i)), Outbox::AppendResult::Ok);
  d.server.focusEvent = "null";
  d.http.advancePerRequestMs = 5000;  // Open budget 12 s: only a few requests fit.
  ResumeDecision decision;
  const SessionReport report = open(decision);
  EXPECT_EQ(report.outcome, SessionOutcome::BudgetExhausted);
  EXPECT_LE(d.http.log.size(), 3u);
  EXPECT_GT(d.outbox->depth(), 0u);
  // Running out of time is not a failure: no backoff.
  EXPECT_EQ(d.store->state().failures, 0);
  EXPECT_EQ(d.engine->gate(SessionKind::Open), Gate::Go);

  d.http.advancePerRequestMs = 0;
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.outbox->depth(), 0u);
  EXPECT_EQ(d.server.recorded.size(), 25u);
}

TEST_F(SyncEngineTest, BudgetsMatchTheDesign) {
  EngineConfig config;
  EXPECT_EQ(config.openBudgetMs, 12000u);
  EXPECT_EQ(config.sleepBudgetMs, 15000u);
  EXPECT_EQ(config.manualBudgetMs, 120000u);
  EXPECT_EQ(config.maxBodyBytes, 8192u);
  EXPECT_EQ(config.maxEventsPerBatch, 20);
  EXPECT_EQ(config.maxPullPages, 4);
}

TEST_F(SyncEngineTest, EachKindGetsItsOwnBudget) {
  // 4 s per request: the 12 s open budget dies quickly, the 120 s manual one does not.
  for (int i = 0; i < 25; ++i) ASSERT_EQ(d.appendRaw("e" + std::to_string(i)), Outbox::AppendResult::Ok);
  d.http.advancePerRequestMs = 4000;
  EXPECT_EQ(manual().outcome, SessionOutcome::Ok);
  EXPECT_EQ(d.outbox->depth(), 0u);
}

// --- Bookkeeping ------------------------------------------------------------------

TEST_F(SyncEngineTest, StatusAndDiagnosticsAreStructured) {
  ASSERT_EQ(d.appendRaw("a"), Outbox::AppendResult::Ok);
  Reply reply = apiError(500, "boom");
  reply.requestId = "abc-123";
  d.http.script.push_back(reply);
  manual();
  const EngineStatus st = d.engine->status();
  EXPECT_TRUE(st.paired);
  EXPECT_EQ(st.depth, 1u);
  EXPECT_EQ(st.diag.stage, SyncStage::Pull);
  EXPECT_EQ(st.diag.httpStatus, 500);
  EXPECT_STREQ(st.diag.code, "boom");
  EXPECT_STREQ(st.diag.requestId, "abc-123");
  EXPECT_GT(st.nextAttemptEpoch, kNow);
  d.crashAndBoot();
  EXPECT_STREQ(d.engine->diagnostics().code, "boom");
  EXPECT_STREQ(d.engine->diagnostics().requestId, "abc-123");
}

TEST_F(SyncEngineTest, NoFileIsLeftOpen) {
  ASSERT_EQ(d.appendRaw("a"), Outbox::AppendResult::Ok);
  manual();
  EXPECT_LE(d.fs.peakOpen(), 4) << "the device glue keeps a pool of four handles";
}

TEST_F(SyncEngineTest, EngineMemoryStaysWithinTheBudgetNote) {
  EXPECT_LT(sizeof(RemoteEvent) + sizeof(BookLedger) + sizeof(Batch), 4u * 1024);
  EXPECT_LT(sizeof(OutboxEntry) * Outbox::kMaxLiveEvents, 3200u);
}
