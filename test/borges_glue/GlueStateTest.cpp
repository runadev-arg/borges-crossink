#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "GlueState.h"

using namespace borges;
using namespace borges::glue;

namespace {

// Just enough IFs: whole files in memory, flush-free durability.
class MemFs : public IFs {
 public:
  std::map<std::string, std::string> files;
  bool failReads = false;

  Handle open(const char* path, Mode mode) override {
    if (mode == Mode::Read && (failReads || !files.count(path))) return kInvalid;
    if (mode == Mode::WriteTrunc) files[path].clear();
    if (mode == Mode::Append) files[path];
    open_.push_back({path, mode == Mode::Append ? files[path].size() : 0});
    return static_cast<Handle>(open_.size() - 1);
  }
  int32_t read(Handle h, void* dst, size_t len) override {
    auto& o = open_[h];
    const auto& data = files[o.path];
    const size_t avail = o.pos < data.size() ? data.size() - o.pos : 0;
    const size_t n = len < avail ? len : avail;
    memcpy(dst, data.data() + o.pos, n);
    o.pos += n;
    return static_cast<int32_t>(n);
  }
  int32_t write(Handle h, const void* src, size_t len) override {
    auto& o = open_[h];
    auto& data = files[o.path];
    if (data.size() < o.pos + len) data.resize(o.pos + len);
    memcpy(&data[o.pos], src, len);
    o.pos += len;
    return static_cast<int32_t>(len);
  }
  bool seek(Handle h, uint32_t pos) override {
    open_[h].pos = pos;
    return true;
  }
  int64_t size(Handle h) override { return static_cast<int64_t>(files[open_[h].path].size()); }
  bool flush(Handle) override { return true; }
  void close(Handle) override {}
  bool exists(const char* path) override { return files.count(path) != 0; }
  bool remove(const char* path) override {
    files.erase(path);
    return true;
  }
  bool rename(const char* from, const char* to) override {
    files[to] = files[from];
    files.erase(from);
    return true;
  }
  bool mkdir(const char*) override { return true; }

 private:
  struct Open {
    std::string path;
    size_t pos;
  };
  std::vector<Open> open_;
};

WallTime wall(int64_t epoch, TimePrecision p = TimePrecision::Exact) {
  WallTime w;
  w.epochSec = epoch;
  w.precision = p;
  return w;
}

OpenInputs goodOpen() {
  OpenInputs in;
  in.isEpub = true;
  in.paired = true;
  in.hasSavedWifi = true;
  return in;
}

}  // namespace

TEST(LoopGuard, NeverMoreThanTwoSessionsThatDoNotComeBack) {
  GlueState s;
  EXPECT_TRUE(armSession(s, PendingKind::OpenBook));  // Crashes: no finish.
  EXPECT_TRUE(armSession(s, PendingKind::OpenBook));  // Crashes again.
  EXPECT_FALSE(armSession(s, PendingKind::OpenBook)) << "The reader must open with no session.";
  EXPECT_EQ(s.pending, PendingKind::None);
  EXPECT_TRUE(armSession(s, PendingKind::OpenBook)) << "A later open gets a fresh chance.";
}

TEST(LoopGuard, ASessionThatComesBackClearsTheCount) {
  GlueState s;
  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(armSession(s, PendingKind::SleepFlush));
    finishSession(s);
    EXPECT_EQ(s.attempts, 0);
  }
}

TEST(LoopGuard, CountSurvivesAReboot) {
  MemFs fs;
  GlueStore store(fs);
  GlueState s;
  ASSERT_TRUE(armSession(s, PendingKind::SleepFlush));
  ASSERT_TRUE(store.save(s));
  GlueState back;
  ASSERT_TRUE(store.load(back));
  EXPECT_EQ(back.attempts, 1);
  EXPECT_EQ(back.pending, PendingKind::SleepFlush);
}

TEST(GlueStore, MissingFileReadsAsDefaults) {
  MemFs fs;
  GlueStore store(fs);
  GlueState s;
  s.autoOnOpen = false;
  EXPECT_FALSE(store.load(s));
  EXPECT_TRUE(s.autoOnOpen);
  EXPECT_TRUE(s.syncOnSleep);
}

TEST(GlueStore, RoundTripsPreferencesAndThrottle) {
  MemFs fs;
  GlueStore store(fs);
  GlueState s;
  s.autoOnOpen = false;
  s.syncOnSleep = false;
  s.lastOpenPathHash = 0xDEADBEEF;
  s.lastOpenEpoch = 1790000000;
  ASSERT_TRUE(store.save(s));
  GlueState back;
  ASSERT_TRUE(store.load(back));
  EXPECT_FALSE(back.autoOnOpen);
  EXPECT_FALSE(back.syncOnSleep);
  EXPECT_EQ(back.lastOpenPathHash, 0xDEADBEEFu);
  EXPECT_EQ(back.lastOpenEpoch, 1790000000);
}

TEST(Throttle, SameBookWithinTwoMinutesSkipsTheCheck) {
  GlueState s;
  const uint32_t book = pathHash("/a.epub");
  noteOpenSuccess(s, book, wall(1000));
  EXPECT_TRUE(openThrottled(s, book, wall(1119)));
  EXPECT_FALSE(openThrottled(s, book, wall(1120)));
  EXPECT_FALSE(openThrottled(s, pathHash("/b.epub"), wall(1001)));
}

TEST(Throttle, ClockWeCannotTrustNeverThrottles) {
  GlueState s;
  const uint32_t book = pathHash("/a.epub");
  noteOpenSuccess(s, book, wall(1000));
  EXPECT_FALSE(openThrottled(s, book, wall(1001, TimePrecision::Unknown)));
  EXPECT_FALSE(openThrottled(s, book, wall(900))) << "A clock that went backwards.";
  GlueState untouched;
  noteOpenSuccess(untouched, book, wall(1000, TimePrecision::Unknown));
  EXPECT_FALSE(openThrottled(untouched, book, wall(1001)));
}

TEST(Route, EveryReasonNotToRebootAvoidsTheReboot) {
  EXPECT_EQ(routeOpen(goodOpen()), OpenRoute::NetworkSession);

  OpenInputs in = goodOpen();
  in.isEpub = false;
  EXPECT_EQ(routeOpen(in), OpenRoute::Direct);
  in = goodOpen();
  in.paired = false;
  EXPECT_EQ(routeOpen(in), OpenRoute::Direct);

  in = goodOpen();
  in.hasSavedWifi = false;
  EXPECT_EQ(routeOpen(in), OpenRoute::DirectWithHold) << "No known network: no delay at all.";
  in = goodOpen();
  in.throttled = true;
  EXPECT_EQ(routeOpen(in), OpenRoute::DirectWithHold);
  in = goodOpen();
  in.autoOnOpen = false;
  EXPECT_EQ(routeOpen(in), OpenRoute::DirectWithHold);
  in = goodOpen();
  in.needsRelink = true;
  EXPECT_EQ(routeOpen(in), OpenRoute::DirectWithHold);
  in = goodOpen();
  in.backedOff = true;
  EXPECT_EQ(routeOpen(in), OpenRoute::DirectWithHold);
  in = goodOpen();
  in.hasPendingJump = true;
  EXPECT_EQ(routeOpen(in), OpenRoute::DirectWithHold);
}

TEST(SleepFlush, OnlyWhenThereIsSomethingToSendAndTheBatteryCanPay) {
  SleepInputs in;
  in.paired = true;
  in.hasSavedWifi = true;
  in.gateGo = true;
  in.batteryPercent = 80;
  EXPECT_TRUE(shouldFlushBeforeSleep(in));

  SleepInputs low = in;
  low.batteryPercent = kMinSleepBatteryPercent - 1;
  EXPECT_FALSE(shouldFlushBeforeSleep(low));
  SleepInputs off = in;
  off.syncOnSleep = false;
  EXPECT_FALSE(shouldFlushBeforeSleep(off));
  SleepInputs empty = in;
  empty.gateGo = false;
  EXPECT_FALSE(shouldFlushBeforeSleep(empty));
  SleepInputs nested = in;
  nested.alreadyInNetworkBoot = true;
  EXPECT_FALSE(shouldFlushBeforeSleep(nested)) << "A flush boot must not arm another flush.";
}

TEST(Age, PlainWordsBuckets) {
  EXPECT_EQ(ageOf(1000, wall(1030)).bucket, AgeBucket::JustNow);
  EXPECT_EQ(ageOf(1000, wall(1000 + 300)).bucket, AgeBucket::Minutes);
  EXPECT_EQ(ageOf(1000, wall(1000 + 300)).value, 5);
  EXPECT_EQ(ageOf(1000, wall(1000 + 7200)).value, 2);
  EXPECT_EQ(ageOf(1000, wall(1000 + 7200)).bucket, AgeBucket::Hours);
  EXPECT_EQ(ageOf(1000, wall(1000 + 86400 * 3)).bucket, AgeBucket::Days);
  EXPECT_EQ(ageOf(1000, wall(2000, TimePrecision::Unknown)).bucket, AgeBucket::Unknown);
  EXPECT_EQ(ageOf(5000, wall(1000)).bucket, AgeBucket::Unknown) << "A future stamp says nothing.";
  EXPECT_EQ(ageOf(0, wall(1000)).bucket, AgeBucket::Unknown);
}

TEST(IsoEpoch, ParsesWhatTheServerSends) {
  int64_t e = 0;
  ASSERT_TRUE(parseIsoEpoch("2026-11-05T10:00:00Z", e));
  EXPECT_EQ(e, 1793872800);
  ASSERT_TRUE(parseIsoEpoch("2026-11-05T10:00:00.123Z", e));
  EXPECT_EQ(e, 1793872800);
  ASSERT_TRUE(parseIsoEpoch("2026-11-05T07:00:00-03:00", e));
  EXPECT_EQ(e, 1793872800);
  EXPECT_FALSE(parseIsoEpoch("ayer", e));
  EXPECT_FALSE(parseIsoEpoch("2026-13-05T10:00:00Z", e));
  EXPECT_FALSE(parseIsoEpoch(nullptr, e));
}

TEST(PositionApply, PageAlwaysFitsItsCount) {
  SavedPosition p;
  ASSERT_TRUE(normalizeForSave(3, 12, 5, 10, p));
  EXPECT_EQ(p.page, 12);
  EXPECT_EQ(p.pageCount, 13);
  ASSERT_TRUE(normalizeForSave(3, -1, 0, 10, p));
  EXPECT_EQ(p.page, 0);
  EXPECT_EQ(p.pageCount, 1);
  ASSERT_TRUE(normalizeForSave(0, 70000, 70000, 1, p));
  EXPECT_LE(p.page, 0xFFFE);
  EXPECT_LE(p.pageCount, 0xFFFF);
  EXPECT_LT(p.page, p.pageCount);
}

TEST(PositionApply, ASpineOutsideTheBookIsNotAPosition) {
  SavedPosition p;
  EXPECT_FALSE(normalizeForSave(10, 0, 1, 10, p));
  EXPECT_FALSE(normalizeForSave(-1, 0, 1, 10, p));
  EXPECT_FALSE(normalizeForSave(0, 0, 1, 0, p));
}

TEST(PositionApply, JumpPrefersXpointerThenPercentage) {
  JumpRequest j;
  ASSERT_TRUE(planJump(true, "/body/p[1]", true, 50000000, j));
  EXPECT_STREQ(j.xpointer, "/body/p[1]");
  EXPECT_FLOAT_EQ(j.fraction, 0.5f);

  ASSERT_TRUE(planJump(false, "/cut", true, 25000000, j));
  EXPECT_STREQ(j.xpointer, "") << "An unusable xpointer must not reach the mapper.";
  EXPECT_FLOAT_EQ(j.fraction, 0.25f);

  ASSERT_TRUE(planJump(true, "", true, 200000000, j));
  EXPECT_FLOAT_EQ(j.fraction, 1.0f);

  EXPECT_FALSE(planJump(false, "", false, 0, j)) << "Nothing to go to: the reader stays.";
}

TEST(ClipTimes, FindsTheStampAndNothingElse) {
  MemFs fs;
  ClipTimes times(fs);
  const char* path = "/cache/borges_clip_times.bin";
  EXPECT_EQ(times.lookup(path, 1, 100), 0) << "No file: no time, no failure.";
  ClipStamp a;
  a.spine = 1;
  a.uptime = 100;
  a.epochMs = 1790000000000;
  ASSERT_TRUE(times.append(path, a));
  ClipStamp b = a;
  b.spine = 2;
  b.epochMs = 1790000001000;
  ASSERT_TRUE(times.append(path, b));
  EXPECT_EQ(times.lookup(path, 1, 100), 1790000000000);
  EXPECT_EQ(times.lookup(path, 2, 100), 1790000001000);
  EXPECT_EQ(times.lookup(path, 1, 101), 0);
}

TEST(ClipTimes, ATornTailIsIgnored) {
  MemFs fs;
  ClipTimes times(fs);
  const char* path = "/t";
  ClipStamp a;
  a.spine = 1;
  a.uptime = 7;
  a.epochMs = 5;
  ASSERT_TRUE(times.append(path, a));
  fs.files[path] += "xx";
  EXPECT_EQ(times.lookup(path, 1, 7), 5);
}

TEST(ClipIdentity, StableAndSpecific) {
  EXPECT_EQ(clipIdentity(1, 2, 3), clipIdentity(1, 2, 3));
  EXPECT_NE(clipIdentity(1, 2, 3), clipIdentity(2, 2, 3));
  EXPECT_NE(clipIdentity(1, 2, 3), clipIdentity(1, 3, 3));
  EXPECT_NE(bookmarkIdentity(1, 2), bookmarkIdentity(1, 3));
}
