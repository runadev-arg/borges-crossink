#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "GlueState.h"
#include "LegacyMigration.h"
#include "Positions.h"

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

TEST(LoopGuard, TheSecondPressAfterADeadSessionOpensTheBook) {
  GlueState s;
  EXPECT_TRUE(armSession(s, PendingKind::OpenBook));  // Crashes: no finish.
  EXPECT_FALSE(armSession(s, PendingKind::OpenBook)) << "The reader must open with no session.";
  EXPECT_EQ(s.pending, PendingKind::None);
  EXPECT_FALSE(armSession(s, PendingKind::OpenBook)) << "The cooldown has one more to go.";
  EXPECT_TRUE(armSession(s, PendingKind::OpenBook)) << "A later open gets a fresh chance.";
}

TEST(LoopGuard, ASessionThatAlwaysPanicsCostsLessAndLess) {
  GlueState s;
  int armed = 0;
  for (int open = 0; open < 80; ++open) {
    if (armSession(s, PendingKind::OpenBook)) ++armed;
  }
  // One session, then 2, 4, 8, 16, 16... skipped.
  EXPECT_LE(armed, 8);
  EXPECT_EQ(s.cooldownLevel, kMaxCooldownLevel);
  finishSession(s);
  EXPECT_EQ(s.cooldownLevel, 0);
  EXPECT_EQ(s.skipsLeft, 0);
  EXPECT_TRUE(armSession(s, PendingKind::OpenBook));
}

TEST(Route, AnOpenTriesThroughBackoffEveryFewMinutes) {
  GlueState s;
  EXPECT_TRUE(openOutlastsBackoff(s, wall(1700000000)));
  s.lastOpenTryEpoch = 1700000000;
  EXPECT_FALSE(openOutlastsBackoff(s, wall(1700000000 + 60)));
  EXPECT_TRUE(openOutlastsBackoff(s, wall(1700000000 + kOpenBackoffCapSec)));
  EXPECT_TRUE(openOutlastsBackoff(s, wall(1700000000 - 10))) << "A clock that went back proves nothing.";
  EXPECT_TRUE(openOutlastsBackoff(s, wall(1700000060, TimePrecision::Unknown)));
}

TEST(Route, ALowBatteryOpensWithoutASessionButHolds) {
  OpenInputs in = goodOpen();
  in.batteryPercent = 14;
  EXPECT_EQ(routeOpen(in), OpenRoute::DirectWithHold);
  in.batteryPercent = 15;
  EXPECT_EQ(routeOpen(in), OpenRoute::NetworkSession);
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
  low.batteryPercent = kMinSessionBatteryPercent - 1;
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
  ClipStamp found;
  EXPECT_FALSE(times.lookup(path, 1, 100, found)) << "No file: no time, no failure.";
  ClipStamp a;
  a.spine = 1;
  a.uptime = 100;
  a.epochMs = 1790000000000;
  ASSERT_TRUE(times.append(path, a));
  ClipStamp b = a;
  b.spine = 2;
  b.epochMs = 1790000001000;
  ASSERT_TRUE(times.append(path, b));
  ASSERT_TRUE(times.lookup(path, 1, 100, found));
  EXPECT_EQ(found.epochMs, 1790000000000);
  EXPECT_EQ(found.visibleOffset, kNoVisibleOffset);
  ASSERT_TRUE(times.lookup(path, 2, 100, found));
  EXPECT_EQ(found.epochMs, 1790000001000);
  EXPECT_FALSE(times.lookup(path, 1, 101, found));
}

TEST(ClipTimes, KeepsTheVisibleOffset) {
  MemFs fs;
  ClipTimes times(fs);
  const char* path = "/o.bin";
  ClipStamp stamp;
  stamp.spine = 3;
  stamp.uptime = 9;
  stamp.visibleOffset = 123456;
  ASSERT_TRUE(times.append(path, stamp));
  ClipStamp found;
  ASSERT_TRUE(times.lookup(path, 3, 9, found));
  EXPECT_EQ(found.visibleOffset, 123456u);
  EXPECT_EQ(found.epochMs, 0);
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
  ClipStamp found;
  ASSERT_TRUE(times.lookup(path, 1, 7, found));
  EXPECT_EQ(found.epochMs, 5);
}

TEST(StaySource, LedgerOnlyWhenItStillDescribesThisPlace) {
  Position finalized;
  finalized.setPercent(40);
  strcpy(finalized.xpointer, "/body/p[3]");
  Position local;
  local.setPercent(40.4);
  EXPECT_EQ(chooseStaySource(true, finalized, local), StaySource::Ledger);
  EXPECT_EQ(chooseStaySource(false, finalized, local), StaySource::MapSavedPage);
  local.setPercent(42);
  EXPECT_EQ(chooseStaySource(true, finalized, local), StaySource::MapSavedPage);
  local = Position{};
  EXPECT_EQ(chooseStaySource(true, finalized, local), StaySource::MapSavedPage);
  local.setPercent(40);
  finalized.xpointer[0] = '\0';
  EXPECT_EQ(chooseStaySource(true, finalized, local), StaySource::MapSavedPage);
}

namespace {
class FakeDirs : public IDirLister {
 public:
  std::vector<std::string> names;
  bool fail = false;
  bool listDirectories(const char*, size_t limit, std::vector<std::string>& out) override {
    if (fail) return false;
    for (const auto& name : names) {
      if (out.size() < limit) out.push_back(name);
    }
    return true;
  }
};

// Fails a write once, after `countdown` successful ones, as a card that fills up.
class FlakyFs : public MemFs {
 public:
  int countdown = -1;
  int32_t write(Handle h, const void* src, size_t len) override {
    if (countdown == 0) return -1;
    if (countdown > 0) --countdown;
    return MemFs::write(h, src, len);
  }
  // The old folder exists as a key too: MemFs has no directories of its own.
  bool mkdir(const char* path) override {
    files[path];
    return true;
  }
};

void seedOldCard(MemFs& fs, FakeDirs& dirs) {
  fs.files["/.borges"];
  fs.files["/.borges/settings.json"] = "{\"language\":\"ES\"}";
  fs.files["/.borges/wifi.json"] = "{\"credentials\":[]}";
  fs.files["/.borges/recent.json"] = "{\"books\":[]}";
  fs.files["/.borges/epub_123/progress.bin"] = "\x02\x00\x05\x00\x10\x00";
  fs.files["/.borges/epub_456/progress.bin.bak"] = "bak";
  fs.files["/.borges/xtc_9/progress.bin"] = "xtc!";
  fs.files["/.borges/epub_123/section.bin"] = "not this";
  dirs.names = {"epub_123", "epub_456", "xtc_9", "bookmarks", "borges", "epub_nothing"};
}
}  // namespace

TEST(LegacyImport, CopiesWhatMattersAndLeavesTheOldFolderAlone) {
  FlakyFs fs;
  FakeDirs dirs;
  seedOldCard(fs, dirs);
  const auto before = fs.files;
  EXPECT_EQ(importLegacyData(fs, dirs), LegacyImport::Done);
  EXPECT_EQ(fs.files["/.crosspoint/settings.json"], before.at("/.borges/settings.json"));
  EXPECT_EQ(fs.files["/.crosspoint/wifi.json"], before.at("/.borges/wifi.json"));
  EXPECT_EQ(fs.files["/.crosspoint/recent.json"], before.at("/.borges/recent.json"));
  EXPECT_EQ(fs.files["/.crosspoint/epub_123/progress.bin"], before.at("/.borges/epub_123/progress.bin"));
  EXPECT_EQ(fs.files["/.crosspoint/epub_456/progress.bin"], "bak");
  EXPECT_EQ(fs.files["/.crosspoint/xtc_9/progress.bin"], "xtc!");
  EXPECT_FALSE(fs.exists("/.crosspoint/epub_123/section.bin"));
  EXPECT_FALSE(fs.exists("/.crosspoint/epub_nothing"));
  EXPECT_TRUE(fs.exists(kLegacyImportMarker));
  // Rollback: every old file is there, byte for byte.
  for (const auto& entry : before) EXPECT_EQ(fs.files[entry.first], entry.second) << entry.first;
}

TEST(LegacyImport, NeverOverwritesAndRunsOnce) {
  FlakyFs fs;
  FakeDirs dirs;
  seedOldCard(fs, dirs);
  fs.files["/.crosspoint/settings.json"] = "mine";
  fs.files["/.crosspoint/epub_123/progress.bin"] = "newer";
  EXPECT_EQ(importLegacyData(fs, dirs), LegacyImport::Done);
  EXPECT_EQ(fs.files["/.crosspoint/settings.json"], "mine");
  EXPECT_EQ(fs.files["/.crosspoint/epub_123/progress.bin"], "newer");
  fs.files.erase("/.crosspoint/wifi.json");
  EXPECT_EQ(importLegacyData(fs, dirs), LegacyImport::NothingToDo);
  EXPECT_FALSE(fs.exists("/.crosspoint/wifi.json"));
}

TEST(LegacyImport, NoOldFolderIsNothing) {
  FlakyFs fs;
  FakeDirs dirs;
  EXPECT_EQ(importLegacyData(fs, dirs), LegacyImport::NothingToDo);
  EXPECT_TRUE(fs.files.empty());
}

TEST(LegacyImport, AFailedWriteLeavesNoHalfFileAndResumes) {
  for (int failAfter = 0; failAfter < 8; ++failAfter) {
    FlakyFs fs;
    FakeDirs dirs;
    seedOldCard(fs, dirs);
    fs.countdown = failAfter;
    const LegacyImport first = importLegacyData(fs, dirs);
    if (first == LegacyImport::Incomplete) {
      EXPECT_FALSE(fs.exists(kLegacyImportMarker));
      // Whatever has its final name is whole.
      if (fs.exists("/.crosspoint/settings.json")) {
        EXPECT_EQ(fs.files["/.crosspoint/settings.json"], fs.files["/.borges/settings.json"]);
      }
    }
    fs.countdown = -1;
    if (first == LegacyImport::Incomplete) EXPECT_EQ(importLegacyData(fs, dirs), LegacyImport::Done);
    EXPECT_EQ(fs.files["/.crosspoint/settings.json"], fs.files["/.borges/settings.json"]);
    EXPECT_EQ(fs.files["/.crosspoint/epub_123/progress.bin"], fs.files["/.borges/epub_123/progress.bin"]);
    EXPECT_TRUE(fs.exists(kLegacyImportMarker));
  }
}

TEST(LegacyImport, ListingFailureKeepsItOpen) {
  FlakyFs fs;
  FakeDirs dirs;
  seedOldCard(fs, dirs);
  dirs.fail = true;
  EXPECT_EQ(importLegacyData(fs, dirs), LegacyImport::Incomplete);
  EXPECT_FALSE(fs.exists(kLegacyImportMarker));
}

TEST(LegacyBookmarks, PathMatchesTheOldFirmware) {
  EXPECT_EQ(legacyBookmarkPath("/books/La invencion.de.Morel.epub"),
            "/.borges/bookmarks/books_La invencion.de.Morel.json");
  EXPECT_EQ(legacyBookmarkPath("/a.epub"), "/.borges/bookmarks/a.json");
}

TEST(LegacyBookmarks, Progress) {
  EXPECT_FLOAT_EQ(legacyBookmarkProgress(10, 5), 0.5f);
  EXPECT_FLOAT_EQ(legacyBookmarkProgress(0, 5), 0.0f);
  EXPECT_FLOAT_EQ(legacyBookmarkProgress(4, 9), 1.0f);
}

TEST(LegacyBookmarks, Uuid) {
  uint8_t id[16];
  ASSERT_TRUE(parseUuid("00112233-4455-6677-8899-aabbccddeeff", id));
  EXPECT_EQ(id[0], 0x00);
  EXPECT_EQ(id[7], 0x77);
  EXPECT_EQ(id[15], 0xff);
  EXPECT_FALSE(parseUuid("", id));
  EXPECT_FALSE(parseUuid(nullptr, id));
  EXPECT_FALSE(parseUuid("00112233-4455-6677-8899-aabbccddeefg", id));
  EXPECT_FALSE(parseUuid("0011223344556677-8899-aabbccddeeff00", id));
}

TEST(TrackedReading, RoundTripsOnceAndRejectsATornFile) {
  MemFs fs;
  const char* path = "/t.bin";
  TrackedReading in;
  in.paged = true;
  in.pagesRead = 12;
  in.startedEpoch = 1700000000;
  in.lastTurnEpoch = 1700000999;
  ASSERT_TRUE(saveTrackedReading(fs, path, in));
  TrackedReading out;
  ASSERT_TRUE(takeTrackedReading(fs, path, out));
  EXPECT_TRUE(out.paged);
  EXPECT_EQ(out.pagesRead, 12);
  EXPECT_EQ(out.startedEpoch, 1700000000);
  EXPECT_EQ(out.lastTurnEpoch, 1700000999);
  EXPECT_FALSE(takeTrackedReading(fs, path, out)) << "Single use.";
  ASSERT_TRUE(saveTrackedReading(fs, path, in));
  fs.files[path].resize(10);
  EXPECT_FALSE(takeTrackedReading(fs, path, out));
  EXPECT_FALSE(fs.exists(path));
}

namespace {
class StringSource : public IByteSource {
 public:
  explicit StringSource(std::string text) : text_(std::move(text)) {}
  int32_t read(uint8_t* dst, size_t len) override {
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

TEST(Positions, PicksTheFirstOtherDeviceWithAPlace) {
  StringSource source(R"({"protocol_version":2,"positions":[
    {"device_id":"me","device_name":"X4","percentage":10,"is_current_device":true,"recovery_id":"a"},
    {"device_id":"nothing","percentage":null,"xpointer":null,"is_current_device":false,"recovery_id":"b"},
    {"book_identifier":{"kind":"x"},"device_id":"kobo","device_name":"Kobo","percentage":62.5,
     "xpointer":"/body/p[3]","occurred_at":"2026-11-05T13:00:00.000Z","time_precision":"exact",
     "is_current_device":false,"recovery_id":"progress:kobo:7"},
    {"device_id":"older","percentage":1,"is_current_device":false,"recovery_id":"c"}]})");
  JsonScanner scan(source);
  RemoteEvent event;
  ASSERT_TRUE(parseOtherPosition(scan, event));
  EXPECT_STREQ(event.originName, "Kobo");
  EXPECT_EQ(event.pctMicro, 62500000u);
  EXPECT_TRUE(event.xpointerUsable);
  EXPECT_STREQ(event.eventId, "progress:kobo:7");
  EXPECT_EQ(event.precision, TimePrecision::Exact);
}

TEST(Positions, OnlyThisDeviceIsNoOffer) {
  StringSource source(
      R"({"positions":[{"device_id":"me","percentage":10,"is_current_device":true,"recovery_id":"a"}]})");
  JsonScanner scan(source);
  RemoteEvent event;
  EXPECT_FALSE(parseOtherPosition(scan, event));
}

TEST(ClipIdentity, StableAndSpecific) {
  EXPECT_EQ(clipIdentity(1, 2, 3), clipIdentity(1, 2, 3));
  EXPECT_NE(clipIdentity(1, 2, 3), clipIdentity(2, 2, 3));
  EXPECT_NE(clipIdentity(1, 2, 3), clipIdentity(1, 3, 3));
  EXPECT_NE(bookmarkIdentity(2), bookmarkIdentity(3));
}
