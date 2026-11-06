#include "TestSupport.h"

using namespace borges;
using namespace borges_test;

namespace {

class OutboxTest : public testing::Test {
 protected:
  void SetUp() override { d.boot(); }
  Device d;
  std::string log() const {
    return std::string(kOutboxDir) + "/outbox." + std::to_string(d.outbox->generation()) + ".log";
  }
};

std::vector<std::string> selectIds(Outbox& outbox, size_t maxEvents, size_t maxBytes,
                                   const IHoldFilter* holds = nullptr) {
  Batch batch;
  outbox.selectBatch(maxEvents, maxBytes, holds, nullptr, 0, batch);
  std::vector<std::string> ids;
  for (uint8_t i = 0; i < batch.count; ++i) {
    std::string buffer(64 * 1024, '\0');
    JsonWriter w(buffer.data(), buffer.size());
    w.arrayBegin();
    if (outbox.writeEnvelope(batch.items[i], w) != Outbox::EnvelopeResult::Ok) continue;
    char id[37];
    uuidFormat(batch.items[i].id, id);
    ids.push_back(id);
  }
  return ids;
}

}  // namespace

TEST_F(OutboxTest, AppendIsDurableAndSurvivesReboot) {
  std::string id;
  ASSERT_EQ(d.appendRaw("one", &id), Outbox::AppendResult::Ok);
  EXPECT_EQ(d.outbox->depth(), 1u);
  d.crashAndBoot();
  EXPECT_EQ(d.outbox->depth(), 1u);
  EXPECT_EQ(selectIds(*d.outbox, 20, 8192), std::vector<std::string>{id});
}

TEST_F(OutboxTest, IdsAreCanonicalAndUnique) {
  std::set<std::string> ids;
  for (int i = 0; i < 10; ++i) {
    std::string id;
    ASSERT_EQ(d.appendRaw("e", &id), Outbox::AppendResult::Ok);
    EXPECT_TRUE(isCanonicalUuid(id.c_str())) << id;
    ids.insert(id);
  }
  EXPECT_EQ(ids.size(), 10u);
}

TEST_F(OutboxTest, SequencesIncreaseAndSurviveReboot) {
  ASSERT_EQ(d.appendRaw("a"), Outbox::AppendResult::Ok);
  ASSERT_EQ(d.appendRaw("b"), Outbox::AppendResult::Ok);
  EXPECT_EQ(d.outbox->nextSequence(), 3u);
  d.crashAndBoot();
  EXPECT_EQ(d.outbox->nextSequence(), 3u);
  ASSERT_EQ(d.appendRaw("c"), Outbox::AppendResult::Ok);
  EXPECT_EQ(d.outbox->entryAt(2)->seq, 3u);
}

TEST_F(OutboxTest, SequenceFloorSurvivesCompactionOfAnEmptyQueue) {
  std::string first;
  ASSERT_EQ(d.appendRaw("a", &first), Outbox::AppendResult::Ok);
  const uint32_t offset = d.outbox->entryAt(0)->offset;
  ASSERT_TRUE(d.outbox->ack(&offset, 1));
  ASSERT_TRUE(d.outbox->compact());
  EXPECT_EQ(d.outbox->nextSequence(), 2u);
  d.crashAndBoot();
  EXPECT_EQ(d.outbox->nextSequence(), 2u);
  ASSERT_EQ(d.appendRaw("b"), Outbox::AppendResult::Ok);
  EXPECT_EQ(d.outbox->entryAt(0)->seq, 2u);
}

TEST_F(OutboxTest, BatchBoundsEventsAndBytes) {
  for (int i = 0; i < 30; ++i) ASSERT_EQ(d.appendRaw("e", nullptr, kMd5A, 400), Outbox::AppendResult::Ok);
  Batch batch;
  d.outbox->selectBatch(20, 1000 * 1000, nullptr, nullptr, 0, batch);
  EXPECT_EQ(batch.count, 20u);
  d.outbox->selectBatch(20, 2000, nullptr, nullptr, 0, batch);
  EXPECT_GE(batch.count, 2u);
  EXPECT_LE(batch.wireBytes, 2000u);
}

TEST_F(OutboxTest, OversizedEventIsSelectedAlone) {
  ASSERT_EQ(d.appendRaw("big", nullptr, kMd5A, 20000), Outbox::AppendResult::Ok);
  ASSERT_EQ(d.appendRaw("next"), Outbox::AppendResult::Ok);
  Batch batch;
  d.outbox->selectBatch(20, 8192, nullptr, nullptr, 0, batch);
  EXPECT_EQ(batch.count, 1u);
  EXPECT_GT(batch.wireBytes, 8192u);
}

TEST_F(OutboxTest, HeldProgressIsSkippedButCounted) {
  std::string progress, other;
  ASSERT_EQ(d.appendRaw("p", &progress, kMd5A, 0, EventKind::Progress), Outbox::AppendResult::Ok);
  ASSERT_EQ(d.appendRaw("a", &other, kMd5A), Outbox::AppendResult::Ok);
  HoldSet holds;
  holds.add(Device::book().md5);
  EXPECT_EQ(selectIds(*d.outbox, 20, 8192, &holds), std::vector<std::string>{other});
  EXPECT_EQ(d.outbox->depth(), 2u);
  EXPECT_EQ(d.outbox->sendableDepth(&holds), 1u);
  holds.remove(Device::book().md5);
  EXPECT_EQ(d.outbox->sendableDepth(&holds), 2u);
}

TEST_F(OutboxTest, ReplaceWorksUntilTheFirstAttemptThenTheEventIsImmutable) {
  std::string first, second, third;
  ASSERT_EQ(d.appendRaw("p1", &first, kMd5A, 0, EventKind::Progress, true), Outbox::AppendResult::Ok);
  ASSERT_EQ(d.appendRaw("p2", &second, kMd5A, 0, EventKind::Progress, true), Outbox::AppendResult::Ok);
  EXPECT_EQ(d.outbox->depth(), 1u);
  EXPECT_EQ(selectIds(*d.outbox, 20, 8192), std::vector<std::string>{second});

  Batch batch;
  d.outbox->selectBatch(20, 8192, nullptr, nullptr, 0, batch);
  ASSERT_TRUE(d.outbox->markAttempted(batch));

  ASSERT_EQ(d.appendRaw("p3", &third, kMd5A, 0, EventKind::Progress, true), Outbox::AppendResult::Ok);
  EXPECT_EQ(d.outbox->depth(), 2u);
  EXPECT_EQ(selectIds(*d.outbox, 20, 8192), (std::vector<std::string>{second, third}));

  // The attempted flag and the replacement survive a reboot.
  d.crashAndBoot();
  EXPECT_EQ(d.outbox->depth(), 2u);
  EXPECT_EQ(selectIds(*d.outbox, 20, 8192), (std::vector<std::string>{second, third}));
  EXPECT_TRUE(d.outbox->entryAt(0)->attempted);
  EXPECT_FALSE(d.outbox->entryAt(1)->attempted);
}

TEST_F(OutboxTest, ReplaceIsPerBook) {
  ASSERT_EQ(d.appendRaw("a", nullptr, kMd5A, 0, EventKind::Progress, true), Outbox::AppendResult::Ok);
  ASSERT_EQ(d.appendRaw("b", nullptr, kMd5B, 0, EventKind::Progress, true), Outbox::AppendResult::Ok);
  EXPECT_EQ(d.outbox->depth(), 2u);
}

TEST_F(OutboxTest, FullQueueRefusesInsteadOfGrowing) {
  for (int i = 0; i < Outbox::kMaxLiveEvents; ++i) ASSERT_EQ(d.appendRaw("e"), Outbox::AppendResult::Ok);
  EXPECT_EQ(d.appendRaw("over"), Outbox::AppendResult::Full);
  EXPECT_EQ(d.outbox->depth(), Outbox::kMaxLiveEvents);
  // Replacing a waiting progress event does not grow it, so that still works.
  d.boot();
  ASSERT_EQ(d.outbox->depth(), Outbox::kMaxLiveEvents);
}

TEST_F(OutboxTest, TooLargeIsRefused) {
  EXPECT_EQ(d.appendRaw("x", nullptr, kMd5A, Outbox::kMaxRecordBytes), Outbox::AppendResult::TooLarge);
  EXPECT_EQ(d.outbox->depth(), 0u);
}

TEST_F(OutboxTest, AckRemovesAndSurvives) {
  std::string a, b;
  ASSERT_EQ(d.appendRaw("a", &a), Outbox::AppendResult::Ok);
  ASSERT_EQ(d.appendRaw("b", &b), Outbox::AppendResult::Ok);
  const uint32_t offset = d.outbox->entryAt(0)->offset;
  ASSERT_TRUE(d.outbox->ack(&offset, 1));
  EXPECT_EQ(d.outbox->depth(), 1u);
  d.crashAndBoot();
  EXPECT_EQ(selectIds(*d.outbox, 20, 8192), std::vector<std::string>{b});
}

TEST_F(OutboxTest, CompactionKeepsIdsSequencesAndAttempts) {
  std::vector<std::string> ids(4);
  for (auto& id : ids) ASSERT_EQ(d.appendRaw("e", &id, kMd5A, 2000), Outbox::AppendResult::Ok);
  Batch batch;
  d.outbox->selectBatch(1, 8192, nullptr, nullptr, 0, batch);
  ASSERT_TRUE(d.outbox->markAttempted(batch));
  const uint32_t dropped = d.outbox->entryAt(3)->offset;
  ASSERT_TRUE(d.outbox->ack(&dropped, 1));
  const uint64_t seqBefore = d.outbox->entryAt(2)->seq;
  const uint32_t gen = d.outbox->generation();
  const uint32_t sizeBefore = d.outbox->logBytes();

  ASSERT_TRUE(d.outbox->compact());
  EXPECT_EQ(d.outbox->generation(), gen + 1);
  EXPECT_LT(d.outbox->logBytes(), sizeBefore);
  EXPECT_FALSE(d.fs.has(std::string(kOutboxDir) + "/outbox." + std::to_string(gen) + ".log"));

  const auto check = [&]() {
    ASSERT_EQ(d.outbox->depth(), 3u);
    EXPECT_TRUE(d.outbox->entryAt(0)->attempted);
    EXPECT_FALSE(d.outbox->entryAt(1)->attempted);
    EXPECT_EQ(d.outbox->entryAt(2)->seq, seqBefore);
    EXPECT_EQ(selectIds(*d.outbox, 3, 64 * 1024), (std::vector<std::string>{ids[0], ids[1], ids[2]}));
  };
  check();
  d.crashAndBoot();
  check();
}

TEST_F(OutboxTest, RenumberKeepsIdsAndOrderAndGoesAboveFloorAndMax) {
  std::vector<std::string> ids(3);
  for (auto& id : ids) ASSERT_EQ(d.appendRaw("e", &id), Outbox::AppendResult::Ok);
  Batch batch;
  d.outbox->selectBatch(1, 8192, nullptr, nullptr, 0, batch);
  ASSERT_TRUE(d.outbox->markAttempted(batch));
  const uint32_t proven = batch.items[0].offset;
  ASSERT_TRUE(d.outbox->renumber(&proven, 1, 100));
  const auto check = [&]() {
    EXPECT_EQ(d.outbox->entryAt(0)->seq, 100u);
    EXPECT_EQ(d.outbox->entryAt(1)->seq, 101u);  // Never attempted: free to move, and must, to keep the order.
    EXPECT_EQ(d.outbox->entryAt(2)->seq, 102u);
    EXPECT_EQ(d.outbox->nextSequence(), 103u);
    EXPECT_EQ(selectIds(*d.outbox, 3, 8192), ids);
  };
  check();
  d.crashAndBoot();
  check();
  ASSERT_TRUE(d.outbox->compact());
  check();
}

TEST_F(OutboxTest, RenumberLeavesUnprovenAttemptedEventsAlone) {
  std::vector<std::string> ids(2);
  for (auto& id : ids) ASSERT_EQ(d.appendRaw("e", &id), Outbox::AppendResult::Ok);
  Batch batch;
  d.outbox->selectBatch(2, 8192, nullptr, nullptr, 0, batch);
  ASSERT_TRUE(d.outbox->markAttempted(batch));
  const uint32_t proven = batch.items[1].offset;
  ASSERT_TRUE(d.outbox->renumber(&proven, 1, 50));
  EXPECT_EQ(d.outbox->entryAt(0)->seq, 1u);
  EXPECT_EQ(d.outbox->entryAt(1)->seq, 50u);
}

TEST_F(OutboxTest, ResetKeepsTheSequenceHighWater) {
  ASSERT_EQ(d.appendRaw("a"), Outbox::AppendResult::Ok);
  ASSERT_EQ(d.appendRaw("b"), Outbox::AppendResult::Ok);
  ASSERT_TRUE(d.outbox->reset());
  EXPECT_EQ(d.outbox->depth(), 0u);
  EXPECT_EQ(d.outbox->nextSequence(), 3u);
  d.crashAndBoot();
  EXPECT_EQ(d.outbox->depth(), 0u);
  EXPECT_EQ(d.outbox->nextSequence(), 3u);
}

TEST_F(OutboxTest, TornTailIsQuarantinedAndLaterAppendsAreReachable) {
  std::string a;
  ASSERT_EQ(d.appendRaw("a", &a), Outbox::AppendResult::Ok);
  // A crash in the middle of an append: half a record on disk.
  std::string partial = d.fs.content(log()).substr(0, 40);
  d.fs.put(log(), d.fs.content(log()) + partial);
  d.boot();
  EXPECT_EQ(d.outbox->depth(), 1u);
  EXPECT_EQ(d.outbox->quarantinedRecords(), 1u);
  EXPECT_TRUE(d.fs.has(std::string(kOutboxDir) + "/outbox.bad"));
  std::string b;
  ASSERT_EQ(d.appendRaw("b", &b), Outbox::AppendResult::Ok);
  d.crashAndBoot();
  EXPECT_EQ(selectIds(*d.outbox, 20, 8192), (std::vector<std::string>{a, b}));
}

TEST_F(OutboxTest, GarbageBetweenRecordsDoesNotHideTheOnesAfter) {
  std::string a, b;
  ASSERT_EQ(d.appendRaw("a", &a), Outbox::AppendResult::Ok);
  const std::string afterA = d.fs.content(log());
  ASSERT_EQ(d.appendRaw("b", &b), Outbox::AppendResult::Ok);
  const std::string whole = d.fs.content(log());
  // The torn record claims a length that runs across the good record behind it.
  std::string torn = whole.substr(afterA.size(), 12);
  d.fs.put(log(), afterA + torn + whole.substr(afterA.size()));
  d.boot();
  EXPECT_EQ(selectIds(*d.outbox, 20, 8192), (std::vector<std::string>{a, b}));
}

TEST_F(OutboxTest, TransientReadFailureDoesNotDiscardAnything) {
  std::string a;
  ASSERT_EQ(d.appendRaw("a", &a), Outbox::AppendResult::Ok);
  d.fs.failOpen(log());
  Outbox failing(d.fs, d.random, kOutboxDir);
  EXPECT_FALSE(failing.open());
  d.fs.failOpen("");
  d.boot();
  EXPECT_EQ(d.outbox->depth(), 1u);
}

TEST_F(OutboxTest, AReadErrorDuringTheScanIsNotCorruption) {
  std::string a, b;
  ASSERT_EQ(d.appendRaw("a", &a), Outbox::AppendResult::Ok);
  ASSERT_EQ(d.appendRaw("b", &b), Outbox::AppendResult::Ok);
  d.fs.failReadsAfter(log(), 20);
  Outbox failing(d.fs, d.random, kOutboxDir);
  EXPECT_FALSE(failing.open());
  EXPECT_FALSE(d.fs.has(std::string(kOutboxDir) + "/outbox.bad"));
  d.fs.failReadsAfter("", 0);
  d.boot();
  EXPECT_EQ(selectIds(*d.outbox, 20, 8192), (std::vector<std::string>{a, b}));
}

TEST_F(OutboxTest, AReadErrorDuringCompactionKeepsTheOldGeneration) {
  std::string a;
  ASSERT_EQ(d.appendRaw("a", &a), Outbox::AppendResult::Ok);
  const uint32_t gen = d.outbox->generation();
  d.fs.failReadsAfter(log(), 0);
  EXPECT_FALSE(d.outbox->compact());
  d.fs.failReadsAfter("", 0);
  EXPECT_EQ(d.outbox->generation(), gen);
  EXPECT_FALSE(d.fs.has(std::string(kOutboxDir) + "/outbox.bad"));
  d.crashAndBoot();
  EXPECT_EQ(selectIds(*d.outbox, 20, 8192), std::vector<std::string>{a});
}

TEST_F(OutboxTest, QuarantineMovesTheBytesAndRemovesTheEvent) {
  ASSERT_EQ(d.appendRaw("poison-text"), Outbox::AppendResult::Ok);
  const uint32_t offset = d.outbox->entryAt(0)->offset;
  ASSERT_TRUE(d.outbox->quarantine(offset));
  EXPECT_EQ(d.outbox->depth(), 0u);
  EXPECT_NE(d.fs.content(std::string(kOutboxDir) + "/outbox.bad").find("poison-text"), std::string::npos);
  d.crashAndBoot();
  EXPECT_EQ(d.outbox->depth(), 0u);
}

TEST_F(OutboxTest, BadFileIsCapped) {
  for (int i = 0; i < 5; ++i) {
    ASSERT_EQ(d.appendRaw("e", nullptr, kMd5A, 30000), Outbox::AppendResult::Ok);
    ASSERT_TRUE(d.outbox->quarantine(d.outbox->entryAt(0)->offset));
  }
  EXPECT_LE(d.fs.content(std::string(kOutboxDir) + "/outbox.bad").size(), Outbox::kMaxBadBytes);
}
