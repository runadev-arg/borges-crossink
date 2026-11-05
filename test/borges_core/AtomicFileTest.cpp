#include "AtomicFile.h"
#include "TestSupport.h"

using namespace borges;
using namespace borges_test;

namespace {
std::string load(AtomicFile& f) {
  uint8_t buf[256];
  size_t len = 0;
  if (!f.load(buf, sizeof(buf), len)) return "<none>";
  return std::string(reinterpret_cast<char*>(buf), len);
}
}  // namespace

TEST(AtomicFile, EmptyHasNothing) {
  MemFs fs;
  AtomicFile f(fs, "/s");
  EXPECT_EQ(load(f), "<none>");
}

TEST(AtomicFile, SaveAndLoad) {
  MemFs fs;
  AtomicFile f(fs, "/s");
  ASSERT_TRUE(f.save("one", 3));
  EXPECT_EQ(load(f), "one");
  ASSERT_TRUE(f.save("two", 3));
  EXPECT_EQ(load(f), "two");
  AtomicFile again(fs, "/s");
  EXPECT_EQ(load(again), "two");
}

TEST(AtomicFile, AlternatesSlots) {
  MemFs fs;
  AtomicFile f(fs, "/s");
  ASSERT_TRUE(f.save("one", 3));
  ASSERT_TRUE(fs.has("/s.a"));
  EXPECT_FALSE(fs.has("/s.b"));
  ASSERT_TRUE(f.save("two", 3));
  ASSERT_TRUE(fs.has("/s.b"));
  ASSERT_TRUE(fs.content("/s.a").find("one") != std::string::npos);
}

TEST(AtomicFile, NeverUsesRename) {
  // The whole point on FAT. A rename here would fail this test: the fake has no such file.
  MemFs fs;
  AtomicFile f(fs, "/s");
  for (int i = 0; i < 5; ++i) ASSERT_TRUE(f.save("x", 1));
  EXPECT_EQ(fs.paths().size(), 2u);
}

TEST(AtomicFile, CorruptNewestFallsBackToPrevious) {
  MemFs fs;
  AtomicFile f(fs, "/s");
  ASSERT_TRUE(f.save("good", 4));
  ASSERT_TRUE(f.save("newer", 5));
  fs.corrupt("/s.b", 17);
  AtomicFile again(fs, "/s");
  EXPECT_EQ(load(again), "good");
  // And the next save overwrites the bad slot, leaving the good one alone.
  ASSERT_TRUE(again.save("third", 5));
  AtomicFile fresh(fs, "/s");
  EXPECT_EQ(load(fresh), "third");
  EXPECT_NE(fs.content("/s.a").find("good"), std::string::npos);
}

TEST(AtomicFile, BothCorruptMeansNothingNotGarbage) {
  MemFs fs;
  AtomicFile f(fs, "/s");
  ASSERT_TRUE(f.save("one", 3));
  ASSERT_TRUE(f.save("two", 3));
  fs.corrupt("/s.a", 5);
  fs.corrupt("/s.b", 5);
  AtomicFile again(fs, "/s");
  EXPECT_EQ(load(again), "<none>");
}

TEST(AtomicFile, AReadErrorIsNotAnEmptyFileAndBlocksSaves) {
  MemFs fs;
  AtomicFile f(fs, "/s");
  ASSERT_TRUE(f.save("one", 3));
  ASSERT_TRUE(f.save("two", 3));
  fs.failReadsAfter("/s.b", 0);
  AtomicFile unreadable(fs, "/s");
  EXPECT_EQ(load(unreadable), "<none>");
  EXPECT_TRUE(unreadable.ioError());
  // Saving now could write a lower sequence than the slot it cannot see.
  EXPECT_FALSE(unreadable.save("three", 5));
  fs.failReadsAfter("", 0);
  AtomicFile fresh(fs, "/s");
  EXPECT_EQ(load(fresh), "two");
  EXPECT_FALSE(fresh.ioError());
}

TEST(AtomicFile, TooSmallBufferDoesNotLoad) {
  MemFs fs;
  AtomicFile f(fs, "/s");
  ASSERT_TRUE(f.save("twelve bytes", 12));
  uint8_t tiny[4];
  size_t len = 0;
  EXPECT_FALSE(f.load(tiny, sizeof(tiny), len));
}

// Kill at every mutating operation of a second save, with each torn-write behaviour:
// load must return either the old payload or the new one, never anything else.
TEST(AtomicFile, CrashAtEveryWriteLeavesOldOrNew) {
  for (TornMode mode : {TornMode::Drop, TornMode::Half, TornMode::Keep}) {
    for (uint64_t kill = 1; kill < 12; ++kill) {
      MemFs fs;
      fs.setTornMode(mode);
      {
        AtomicFile f(fs, "/s");
        ASSERT_TRUE(f.save("first-payload", 13));
        fs.killAtOp(fs.ops() + kill);
        const bool ok = f.save("second-payload!", 15);
        const bool wasKilled = fs.dead();
        fs.reboot();
        AtomicFile again(fs, "/s");
        const std::string seen = load(again);
        if (ok && !wasKilled) {
          EXPECT_EQ(seen, "second-payload!");
        } else {
          EXPECT_TRUE(seen == "first-payload" || seen == "second-payload!") << "kill=" << kill << " saw " << seen;
        }
        // Whatever happened, the next save must work.
        ASSERT_TRUE(again.save("third", 5));
        AtomicFile fresh(fs, "/s");
        EXPECT_EQ(load(fresh), "third");
      }
    }
  }
}
