// Kill the device at every single mutating file operation of a full cycle
// (enqueue -> exchange -> ack -> compact -> enqueue -> exchange), under each model of
// what a crash leaves on the card, then recover and finish. Whatever the crash point,
// the invariants must hold:
//
//   no unacked event lost once durable   every event whose append returned Ok reaches
//                                        the server by the end
//   no acked event resent as new         once an ACK is durable the id never goes out
//                                        again; before that, a resend has the same id,
//                                        body and sequence
//   ids immutable                        an id never changes body
//   sequences unique and increasing      one id per sequence, one sequence per id, and
//                                        strictly increasing inside every request

#include "TestSupport.h"

using namespace borges;
using namespace borges_test;

namespace {

struct Stats {
  uint64_t crashPoints = 0;
  uint64_t crashesHit = 0;
};

class CycleRun {
 public:
  explicit CycleRun(TornMode mode) { d.fs.setTornMode(mode); }

  Device d;
  std::vector<std::string> durableIds;    // Append returned Ok.
  std::set<std::string> ackedDurably;     // Not in the queue anymore after a reboot.
  std::map<std::string, std::string> seqOfId;
  std::map<std::string, std::string> idOfSeq;
  std::vector<std::string> violations;

  void hook() {
    d.http.handler = [this](const RecordedRequest& r) {
      if (r.path == "/api/sync/v2/exchange") {
        JsonDocument doc;
        if (deserializeJson(doc, r.body)) {
          violations.push_back("request is not JSON");
          return d.server.handle(r);
        }
        uint64_t previous = 0;
        for (JsonObject e : doc["events"].as<JsonArray>()) {
          const std::string id = e["client_event_id"].as<std::string>();
          const std::string seq = e["client_sequence"].as<std::string>();
          if (ackedDurably.count(id)) violations.push_back("acked event resent: " + id);
          auto s = seqOfId.find(id);
          if (s != seqOfId.end() && s->second != seq) violations.push_back("sequence changed for " + id);
          auto i = idOfSeq.find(seq);
          if (i != idOfSeq.end() && i->second != id) violations.push_back("sequence reused: " + seq);
          seqOfId[id] = seq;
          idOfSeq[seq] = id;
          const uint64_t value = strtoull(seq.c_str(), nullptr, 10);
          if (value <= previous) violations.push_back("sequences not increasing in a request");
          previous = value;
        }
      }
      return d.server.handle(r);
    };
  }

  // Returns false when the file system died during the step.
  bool appendStep(int index, size_t padding) {
    if (d.fs.dead()) return false;
    std::string id;
    const auto result = d.appendRaw("event-" + std::to_string(index), &id, kMd5A, padding);
    if (result == Outbox::AppendResult::Ok) {
      durableIds.push_back(id);
      return true;
    }
    return false;
  }

  bool sessionStep() {
    if (d.fs.dead()) return false;
    d.engine->runManualSession(nullptr, nullptr, nullptr);
    return !d.fs.dead();
  }

  void recover() {
    d.crashAndBoot();
    hook();
    // Whatever is no longer queued and the server has is durably acked.
    const auto live = liveEvents(*d.outbox);
    std::set<std::string> liveIds;
    for (const auto& kv : live) {
      liveIds.insert(kv.first);
      auto s = seqOfId.find(kv.first);
      if (s != seqOfId.end() && std::to_string(kv.second) != s->second) {
        violations.push_back("sequence changed across a crash for " + kv.first);
      }
    }
    for (const auto& kv : d.server.byId) {
      if (!liveIds.count(kv.first)) ackedDurably.insert(kv.first);
    }
    for (const std::string& id : durableIds) {
      if (!liveIds.count(id) && !d.server.byId.count(id)) violations.push_back("durable event lost: " + id);
    }
  }

  // The scenario, as steps that restart after a crash. Step counters keep what already
  // returned Ok from being redone.
  void run(uint64_t killAt) {
    d.boot();
    hook();
    if (killAt) d.fs.killAtOp(d.fs.ops() + killAt);
    int nextAppend = 0;
    int phase = 0;
    int guard = 0;
    for (;;) {
      ASSERT_LT(++guard, 100) << "scenario does not terminate";
      bool ok = true;
      switch (phase) {
        case 0:  // A few small events.
          while (ok && nextAppend < 3) {
            ok = appendStep(nextAppend, 0);
            if (ok) ++nextAppend;
          }
          break;
        case 1:  // Exchange and ack.
          ok = sessionStep();
          break;
        case 2:  // Enough dead bytes to force a compaction in the next session.
          while (ok && nextAppend < 7) {
            ok = appendStep(nextAppend, 1500);
            if (ok) ++nextAppend;
          }
          break;
        case 3:  // Exchange, ack, compact.
          ok = sessionStep();
          break;
        case 4:  // After the compaction.
          ok = appendStep(nextAppend, 0);
          if (ok) ++nextAppend;
          break;
        case 5:
          ok = sessionStep();
          break;
        default:
          break;
      }
      if (!ok) {
        recover();
        continue;
      }
      if (++phase == 6) break;
    }
    // Final drain, with a healthy disk.
    if (d.fs.dead()) recover();
    for (int i = 0; i < 4 && d.outbox->depth() > 0; ++i) d.engine->runManualSession(nullptr, nullptr, nullptr);
  }
};

void checkInvariants(const CycleRun& run, const std::string& context) {
  EXPECT_EQ(run.violations, std::vector<std::string>{}) << context;
  EXPECT_EQ(run.d.server.violations, std::vector<std::string>{}) << context;
  EXPECT_EQ(run.d.outbox->depth(), 0u) << context;
  // No unacked event lost once durable.
  for (const std::string& id : run.durableIds) EXPECT_EQ(run.d.server.byId.count(id), 1u) << context << " lost " << id;
  // Every text made it, even those whose append crashed and was redone under a new id.
  for (int i = 0; i < 8; ++i) EXPECT_TRUE(run.d.server.hasText("event-" + std::to_string(i))) << context << " event-" << i;
}

uint64_t countOps() {
  CycleRun probe(TornMode::Drop);
  probe.d.boot();
  const uint64_t base = probe.d.fs.ops();
  probe.run(0);
  return probe.d.fs.ops() - base;
}

void matrix(TornMode mode) {
  const uint64_t total = countOps();
  ASSERT_GT(total, 40u) << "the scenario should exercise real IO";
  uint64_t hits = 0;
  for (uint64_t kill = 1; kill <= total + 5; ++kill) {
    CycleRun run(mode);
    run.run(kill);
    if (run.d.server.recorded.size() > 0) ++hits;
    checkInvariants(run, "kill=" + std::to_string(kill));
    if (testing::Test::HasFailure()) {
      FAIL() << "first failing crash point: " << kill << " of " << total;
    }
  }
  // The harness itself must have covered the scenario.
  EXPECT_GE(hits, total);
  std::cout << "[ crash matrix ] " << total << " crash points checked\n";
}

}  // namespace

TEST(CrashMatrix, EveryWriteWithUnflushedBytesDropped) { matrix(TornMode::Drop); }
TEST(CrashMatrix, EveryWriteWithTheLastWriteTorn) { matrix(TornMode::Half); }
TEST(CrashMatrix, EveryWriteWithUnflushedBytesKept) { matrix(TornMode::Keep); }

// A focused sweep over the sequence repair: a crash between the 409 and the retry must
// leave either the old numbers or the complete new ones, and the ids alone in any case.
TEST(CrashMatrix, SequenceRepairIsAllOrNothing) {
  for (TornMode mode : {TornMode::Drop, TornMode::Half, TornMode::Keep}) {
    for (uint64_t kill = 1; kill < 60; ++kill) {
      Device d;
      d.fs.setTornMode(mode);
      d.boot();
      for (int i = 1; i <= 5; ++i) d.server.usedSeq.insert(std::to_string(i));
      std::vector<std::string> ids(3);
      for (int i = 0; i < 3; ++i) ASSERT_EQ(d.appendRaw("r" + std::to_string(i), &ids[static_cast<size_t>(i)]), Outbox::AppendResult::Ok);
      d.fs.killAtOp(d.fs.ops() + kill);
      d.engine->runManualSession(nullptr, nullptr, nullptr);
      const bool crashed = d.fs.dead();
      d.fs.killAtOp(0);
      if (crashed) d.crashAndBoot();
      for (int i = 0; i < 3 && d.outbox->depth() > 0; ++i) d.engine->runManualSession(nullptr, nullptr, nullptr);
      EXPECT_EQ(d.outbox->depth(), 0u) << "kill=" << kill;
      EXPECT_EQ(d.server.violations, std::vector<std::string>{}) << "kill=" << kill;
      for (const std::string& id : ids) EXPECT_EQ(d.server.byId.count(id), 1u) << "kill=" << kill;
      ASSERT_EQ(d.server.recorded.size(), 3u) << "kill=" << kill;
      // Order preserved.
      for (size_t i = 0; i < 3; ++i) EXPECT_EQ(d.server.recorded[i].id, ids[i]) << "kill=" << kill;
      EXPECT_LT(strtoull(d.server.recorded[0].seq.c_str(), nullptr, 10), strtoull(d.server.recorded[1].seq.c_str(), nullptr, 10));
      EXPECT_LT(strtoull(d.server.recorded[1].seq.c_str(), nullptr, 10), strtoull(d.server.recorded[2].seq.c_str(), nullptr, 10));
      if (!crashed) break;
    }
  }
}

// Pull pages: a crash at any point leaves the cursor at a page boundary whose events are
// all applied: the head of a book it has passed is on the card.
TEST(CrashMatrix, CursorNeverPassesWhatWasApplied) {
  for (TornMode mode : {TornMode::Drop, TornMode::Half, TornMode::Keep}) {
    for (uint64_t kill = 1; kill < 120; ++kill) {
      Device d;
      d.fs.setTornMode(mode);
      d.config.pullLimit = 2;
      d.boot();
      for (uint64_t seq = 10; seq < 16; ++seq) {
        const std::string event =
            "{\"event_id\":\"evt-" + std::to_string(seq) + "\",\"server_sequence\":\"" + std::to_string(seq) +
            "\",\"origin_device\":{\"id\":\"" + kOtherId + "\"},\"event_type\":\"progress.changed\","
            "\"book_identifier\":{\"kind\":\"koreader_partial_md5\",\"value\":\"" + kMd5A + "\"},"
            "\"payload\":{\"percentage\":" + std::to_string(seq) + "},\"directive\":\"suggest_resume\",\"directive_metadata\":{}}";
        d.server.addFeedEvent(seq, event);
      }
      d.fs.killAtOp(d.fs.ops() + kill);
      d.engine->runManualSession(nullptr, nullptr, nullptr);
      const bool crashed = d.fs.dead();
      d.fs.killAtOp(0);
      if (crashed) {
        d.crashAndBoot();
        // What the cursor promises must be on the card.
        const uint64_t cursor = d.store->state().cursor;
        if (cursor >= 10) {
          RemoteHead head;
          ASSERT_TRUE(d.engine->remoteHead(Device::book(), head)) << "kill=" << kill;
          EXPECT_GE(head.serverSeq, cursor > 15 ? 15 : cursor) << "kill=" << kill;
        }
      }
      for (int i = 0; i < 4; ++i) d.engine->runManualSession(nullptr, nullptr, nullptr);
      RemoteHead head;
      ASSERT_TRUE(d.engine->remoteHead(Device::book(), head)) << "kill=" << kill;
      EXPECT_EQ(head.serverSeq, 15u) << "kill=" << kill;
      if (!crashed) break;
    }
  }
}

// Finalize with clippings, crashed anywhere: the server ends up with exactly one sync_id
// per clipping and the ledger agrees.
TEST(CrashMatrix, FinalizeNeverDuplicatesAnAnnotation) {
  class Clips : public IClipSource {
   public:
    std::string texts[3] = {"uno", "dos", "tres"};
    bool count(size_t& out) override {
      out = 3;
      return true;
    }
    bool key(size_t i, ClipKey& out) override {
      out.spine = 1;
      out.textOffset = static_cast<uint32_t>(i);
      out.textHash = i + 100;
      return true;
    }
    bool detail(size_t i, ClipDetail& out) override {
      out.text = texts[i].c_str();
      out.page = static_cast<int32_t>(i);
      return true;
    }
  };
  for (TornMode mode : {TornMode::Drop, TornMode::Half, TornMode::Keep}) {
    for (uint64_t kill = 1; kill < 60; ++kill) {
      Device d;
      d.fs.setTornMode(mode);
      d.boot();
      Clips clips;
      FinalizeInput input = d.finalizeInput(30);
      input.clips = &clips;
      d.fs.killAtOp(d.fs.ops() + kill);
      d.engine->finalizeBook(input);
      const bool crashed = d.fs.dead();
      d.fs.killAtOp(0);
      if (crashed) d.crashAndBoot();
      const FinalizeResult again = d.engine->finalizeBook(input);
      EXPECT_TRUE(again.ok) << "kill=" << kill;
      EXPECT_EQ(d.engine->runManualSession(nullptr, nullptr, nullptr).outcome, SessionOutcome::Ok) << "kill=" << kill;
      std::set<std::string> syncIds;
      int annotations = 0;
      for (const auto& e : d.server.recorded) {
        if (e.type != "annotation.created") continue;
        ++annotations;
        JsonDocument doc;
        ASSERT_FALSE(deserializeJson(doc, e.body));
        syncIds.insert(doc["payload"]["sync_id"].as<std::string>());
      }
      EXPECT_EQ(syncIds.size(), 3u) << "kill=" << kill;
      // An event may be queued twice around a crash, but always for the same sync_id.
      EXPECT_GE(annotations, 3) << "kill=" << kill;
      if (!crashed) break;
    }
  }
}
