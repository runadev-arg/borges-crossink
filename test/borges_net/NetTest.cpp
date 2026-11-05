// The pure half of the Borges network layer: rules 6, 7, 8 and 12.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "net/ClockChain.h"
#include "net/DnsChain.h"
#include "net/DnsCodec.h"
#include "net/HttpDate.h"
#include "net/NetFailure.h"
#include "net/NetRecords.h"

using namespace borges;
using namespace borges::net;
using borges_test::FakeCancel;
using borges_test::FakeClock;
using borges_test::MemFs;

namespace {

std::vector<uint8_t> dnsResponse(uint16_t id, const std::vector<std::vector<uint8_t>>& answers, uint8_t rcode = 0,
                                 const char* question = "borges.runadev.com") {
  std::vector<uint8_t> m = {static_cast<uint8_t>(id >> 8),
                            static_cast<uint8_t>(id),
                            0x81,
                            static_cast<uint8_t>(0x80 | rcode),
                            0,
                            1,
                            0,
                            static_cast<uint8_t>(answers.size()),
                            0,
                            0,
                            0,
                            0};
  const char* label = question;
  while (*label) {
    const char* dot = strchr(label, '.');
    const size_t n = dot ? static_cast<size_t>(dot - label) : strlen(label);
    m.push_back(static_cast<uint8_t>(n));
    m.insert(m.end(), label, label + n);
    label += n + (dot ? 1 : 0);
  }
  m.insert(m.end(), {0, 0, 1, 0, 1});
  for (const auto& a : answers) m.insert(m.end(), a.begin(), a.end());
  return m;
}

std::vector<uint8_t> aRecord(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return {0xC0, 12, 0, 1, 0, 1, 0, 0, 1, 44, 0, 4, a, b, c, d};
}

std::vector<uint8_t> cnameRecord() { return {0xC0, 12, 0, 5, 0, 1, 0, 0, 1, 44, 0, 2, 0xC0, 12}; }

}  // namespace

// --- DNS codec ---------------------------------------------------------------------

TEST(NetFailure, AVerificationFailureOnALowHeapIsLowMemory) {
  using namespace borges::net;
  const auto tls = borges::TransportStage::Tls;
  EXPECT_EQ(heapAwareCode(tls, wolfcode::kSigConfirm, 40000), kCodeLowMemory);
  EXPECT_EQ(heapAwareCode(tls, wolfcode::kSigConfirm, kTlsFailureLowHeap), wolfcode::kSigConfirm);
  EXPECT_EQ(heapAwareCode(tls, wolfcode::kNameMismatch, 40000), wolfcode::kNameMismatch);
  EXPECT_EQ(heapAwareCode(borges::TransportStage::Connect, wolfcode::kSigConfirm, 40000), wolfcode::kSigConfirm);
}

TEST(DnsCodec, QueryIsLengthPrefixedAndWellFormed) {
  uint8_t out[kDnsMaxQuery];
  const size_t n = buildDnsQuery("a.bc", 0x1234, out, sizeof(out));
  const uint8_t expected[] = {0,   22,  0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0,
                              1,   'a', 2,    'b',  'c', 0, 0, 1, 0, 1};
  ASSERT_EQ(n, sizeof(expected));
  EXPECT_EQ(memcmp(out, expected, n), 0);
}

TEST(DnsCodec, RefusesInvalidNames) {
  uint8_t out[kDnsMaxQuery];
  EXPECT_EQ(buildDnsQuery("", 1, out, sizeof(out)), 0u);
  EXPECT_EQ(buildDnsQuery(nullptr, 1, out, sizeof(out)), 0u);
  EXPECT_EQ(buildDnsQuery("a..b", 1, out, sizeof(out)), 0u);
  EXPECT_EQ(buildDnsQuery(".a", 1, out, sizeof(out)), 0u);
  EXPECT_EQ(buildDnsQuery(std::string(64, 'a').c_str(), 1, out, sizeof(out)), 0u);
  EXPECT_EQ(buildDnsQuery("a.bc", 1, out, 5), 0u);
}

TEST(DnsCodec, TrailingDotMakesTheSameQuery) {
  uint8_t a[kDnsMaxQuery], b[kDnsMaxQuery];
  const size_t na = buildDnsQuery("a.bc", 1, a, sizeof(a));
  const size_t nb = buildDnsQuery("a.bc.", 1, b, sizeof(b));
  ASSERT_EQ(na, nb);
  EXPECT_EQ(memcmp(a, b, na), 0);
}

TEST(DnsCodec, ParsesFirstARecordPastCname) {
  const auto m = dnsResponse(7, {cnameRecord(), aRecord(104, 21, 0, 166), aRecord(1, 2, 3, 4)});
  uint8_t ip[4];
  ASSERT_EQ(parseDnsAnswer(m.data(), m.size(), 7, "BORGES.runadev.com", ip), DnsParse::Ok);
  EXPECT_EQ(ip[0], 104);
  EXPECT_EQ(ip[3], 166);
}

TEST(DnsCodec, RefusesWhatIsNotTheAnswer) {
  uint8_t ip[4];
  const auto good = dnsResponse(7, {aRecord(1, 2, 3, 4)});
  EXPECT_EQ(parseDnsAnswer(good.data(), 5, 7, "borges.runadev.com", ip), DnsParse::TooShort);
  EXPECT_EQ(parseDnsAnswer(good.data(), good.size(), 8, "borges.runadev.com", ip), DnsParse::BadId);
  EXPECT_EQ(parseDnsAnswer(good.data(), good.size(), 7, "evil.example.com", ip), DnsParse::WrongQuestion);
  auto query = good;
  query[2] = 0x01;
  EXPECT_EQ(parseDnsAnswer(query.data(), query.size(), 7, "borges.runadev.com", ip), DnsParse::NotAResponse);
  const auto nx = dnsResponse(7, {}, 3);
  EXPECT_EQ(parseDnsAnswer(nx.data(), nx.size(), 7, "borges.runadev.com", ip), DnsParse::ServerError);
  const auto empty = dnsResponse(7, {});
  EXPECT_EQ(parseDnsAnswer(empty.data(), empty.size(), 7, "borges.runadev.com", ip), DnsParse::NoAddress);
}

TEST(DnsCodec, EveryTruncationIsRefusedWithoutOverrun) {
  const auto m = dnsResponse(7, {cnameRecord(), aRecord(1, 2, 3, 4)});
  uint8_t ip[4];
  for (size_t n = 0; n + 1 < m.size(); ++n) {
    std::vector<uint8_t> cut(m.begin(), m.begin() + static_cast<std::ptrdiff_t>(n));
    EXPECT_NE(parseDnsAnswer(cut.data(), cut.size(), 7, "borges.runadev.com", ip), DnsParse::Ok) << n;
  }
}

TEST(DnsCodec, ImpossibleAddressesAreNotServers) {
  const uint8_t bad[][4] = {{0, 0, 0, 0}, {127, 0, 0, 1}, {169, 254, 1, 1}, {224, 0, 0, 1}, {255, 255, 255, 255}};
  for (const auto& a : bad) EXPECT_FALSE(plausibleServerAddress(a));
  const uint8_t good[][4] = {{104, 21, 0, 166}, {192, 168, 1, 10}};
  for (const auto& a : good) EXPECT_TRUE(plausibleServerAddress(a));
}

// --- DNS chain (rule 8) ------------------------------------------------------------

namespace {

class ScriptedDns : public IDnsBackend {
 public:
  explicit ScriptedDns(FakeClock& clock) : clock_(clock) {}
  bool gaiOk = false;
  bool tcpOk = false;
  bool lastOk = false;
  uint32_t gaiCostMs = 0;
  int gaiCalls = 0;
  int tcpCalls = 0;
  uint32_t lastGaiBudget = 0;

  bool getaddrinfo(const char*, const uint32_t budgetMs, uint8_t out[4]) override {
    ++gaiCalls;
    lastGaiBudget = budgetMs;
    clock_.mono += gaiCostMs < budgetMs ? gaiCostMs : budgetMs;
    if (!gaiOk) return false;
    const uint8_t ip[4] = {9, 9, 9, 1};
    memcpy(out, ip, 4);
    return true;
  }
  bool dnsOverTcp(const uint8_t[4], const char*, uint32_t, uint8_t out[4]) override {
    ++tcpCalls;
    if (!tcpOk) return false;
    const uint8_t ip[4] = {9, 9, 9, 2};
    memcpy(out, ip, 4);
    return true;
  }
  bool lastGood(const char*, uint8_t out[4]) override {
    if (!lastOk) return false;
    const uint8_t ip[4] = {9, 9, 9, 3};
    memcpy(out, ip, 4);
    return true;
  }

 private:
  FakeClock& clock_;
};

struct ChainFixture {
  FakeClock clock;
  FakeCancel cancel;
  ScriptedDns dns{clock};
  DnsChain chain;
  const Candidates& resolve(const char* host, uint32_t budget = 12000) {
    return chain.resolve(host, budget, clock.monotonicMs(), dns, cancel, clock);
  }
};

}  // namespace

TEST(DnsChainRule8, UsesGetaddrinfoFirstAndKeepsFallbacksBehindIt) {
  ChainFixture f;
  f.dns.gaiOk = true;
  f.dns.lastOk = true;
  const Candidates& c = f.resolve(kProductionHost);
  ASSERT_EQ(c.count, 4);
  EXPECT_EQ(c.items[0].source, DnsSource::Getaddrinfo);
  EXPECT_EQ(c.items[1].source, DnsSource::LastGood);
  EXPECT_EQ(c.items[2].source, DnsSource::Baked);
  EXPECT_EQ(f.dns.tcpCalls, 0);
}

TEST(DnsChainRule8, FallsThroughTcpThenLastGoodThenBaked) {
  ChainFixture f;
  f.dns.tcpOk = true;
  EXPECT_EQ(f.resolve(kProductionHost).items[0].source, DnsSource::DnsOverTcp);
  EXPECT_EQ(f.dns.tcpCalls, 1);

  ChainFixture g;
  g.dns.lastOk = true;
  EXPECT_EQ(g.resolve(kProductionHost).items[0].source, DnsSource::LastGood);
  EXPECT_EQ(g.dns.tcpCalls, 2);

  ChainFixture h;
  const Candidates& c = h.resolve(kProductionHost);
  ASSERT_EQ(c.count, 2);
  EXPECT_EQ(c.items[0].source, DnsSource::Baked);
  EXPECT_EQ(c.items[0].ip[0], 104);
  EXPECT_EQ(c.items[1].ip[0], 172);
}

TEST(DnsChainRule8, BakedAddressesAreOnlyForProduction) {
  ChainFixture f;
  EXPECT_EQ(f.resolve("selfhosted.example.com").count, 0);
  ChainFixture g;
  EXPECT_EQ(g.resolve("BORGES.RUNADEV.COM").count, 2);
}

TEST(DnsChainRule8, AFailureNeverStays) {
  ChainFixture f;
  EXPECT_EQ(f.resolve("selfhosted.example.com").count, 0);
  EXPECT_EQ(f.dns.gaiCalls, 1);
  f.dns.gaiOk = true;
  // At once: no waiting out a memo of the failure.
  const Candidates& c = f.resolve("selfhosted.example.com");
  ASSERT_EQ(c.count, 1);
  EXPECT_EQ(c.items[0].source, DnsSource::Getaddrinfo);
  EXPECT_EQ(f.dns.gaiCalls, 2);
}

TEST(DnsChainRule8, BakedOnlyAnswerDoesNotOutliveTheSession) {
  ChainFixture f;
  f.resolve(kProductionHost);
  EXPECT_EQ(f.dns.gaiCalls, 1);
  f.resolve(kProductionHost);
  EXPECT_EQ(f.dns.gaiCalls, 1);  // The same session does not pay for a dead resolver twice.
  f.clock.mono += kDnsFallbackMemoMs + 1;
  f.dns.gaiOk = true;
  EXPECT_EQ(f.resolve(kProductionHost).items[0].source, DnsSource::Getaddrinfo);
  EXPECT_EQ(f.dns.gaiCalls, 2);
}

TEST(DnsChainRule8, LiveAnswerIsMemoizedForFiveMinutes) {
  ChainFixture f;
  f.dns.gaiOk = true;
  f.resolve(kProductionHost);
  f.clock.mono += kDnsMemoMs - 1;
  f.resolve(kProductionHost);
  EXPECT_EQ(f.dns.gaiCalls, 1);
  f.clock.mono += 2;
  f.resolve(kProductionHost);
  EXPECT_EQ(f.dns.gaiCalls, 2);
  f.resolve("other.example.com");
  EXPECT_EQ(f.dns.gaiCalls, 3);
}

TEST(DnsChainRule8, StaysInsideTheBudget) {
  ChainFixture f;
  f.dns.gaiCostMs = 100000;
  f.resolve(kProductionHost, 12000);
  EXPECT_EQ(f.dns.lastGaiBudget, kGetaddrinfoMaxMs);
  ChainFixture g;
  g.dns.gaiCostMs = 100000;
  const Candidates& c = g.resolve(kProductionHost, 1000);
  EXPECT_EQ(g.dns.lastGaiBudget, 1000u);
  EXPECT_EQ(g.dns.tcpCalls, 0);  // Nothing left for the network, but the baked IPs still come out.
  EXPECT_EQ(c.count, 2);
}

TEST(DnsChainRule8, CancelSkipsTheNetworkButNotTheFallbacks) {
  ChainFixture f;
  f.cancel.flag = true;
  const Candidates& c = f.resolve(kProductionHost);
  EXPECT_EQ(f.dns.gaiCalls, 0);
  EXPECT_EQ(f.dns.tcpCalls, 0);
  EXPECT_EQ(c.count, 2);
}

// --- HTTP date ---------------------------------------------------------------------

namespace {
bool parse(const std::string& s, int64_t& out) { return parseHttpDate(s.data(), s.size(), out); }
}  // namespace

TEST(HttpDate, ParsesImfFixdate) {
  int64_t e = 0;
  ASSERT_TRUE(parse("Sun, 06 Nov 1994 08:49:37 GMT", e));
  EXPECT_EQ(e, 784111777);
  ASSERT_TRUE(parse(" Thu, 05 Nov 2026 13:29:01 GMT\r", e));
  EXPECT_EQ(e, 1793885341);
  ASSERT_TRUE(parse("Thu, 29 Feb 2024 00:00:00 GMT", e));
  EXPECT_EQ(e, 1709164800);
}

TEST(HttpDate, RefusesEverythingElse) {
  int64_t e = 42;
  for (const char* bad : {"", "Sunday, 06-Nov-94 08:49:37 GMT", "Sun Nov  6 08:49:37 1994", "Sun, 06 Nov 1994 08:49:37 UTC",
                          "Mon, 06 Nov 1994 08:49:37 GMT", "Sun, 31 Feb 2026 08:49:37 GMT", "Thu, 29 Feb 2023 00:00:00 GMT",
                          "Sun, 06 Foo 1994 08:49:37 GMT", "Sun, 06 Nov 1994 25:49:37 GMT", "Sun, 06 Nov 19x4 08:49:37 GMT"}) {
    EXPECT_FALSE(parse(bad, e)) << bad;
  }
  EXPECT_EQ(e, 42);
}

TEST(HttpDate, BoundsAre2026To2045) {
  EXPECT_FALSE(epochInBounds(1767225599));
  EXPECT_TRUE(epochInBounds(1767225600));
  EXPECT_TRUE(epochInBounds(2398377599));
  EXPECT_FALSE(epochInBounds(2398377600));
  EXPECT_FALSE(epochInBounds(0));
}

// --- Clock rule (rule 6) -----------------------------------------------------------

namespace {
constexpr int64_t kNow = 1793885341;
constexpr int64_t kOldAnchor = kNow - 86400;
}  // namespace

TEST(ClockRule6, ExactSurvivesOnlyARestartInsideThePowerCycle) {
  EXPECT_EQ(labelAtBoot(ResetKind::SoftwareRestart, TimePrecision::Exact, kNow, kOldAnchor), TimePrecision::Exact);
  EXPECT_EQ(labelAtBoot(ResetKind::DeepSleepWake, TimePrecision::Exact, kNow, kOldAnchor), TimePrecision::Anchored);
  EXPECT_EQ(labelAtBoot(ResetKind::Crash, TimePrecision::Exact, kNow, kOldAnchor), TimePrecision::Anchored);
}

TEST(ClockRule6, NeverLabelsStaleTimeExact) {
  // A retained label with a clock out of bounds is noise from RTC memory.
  EXPECT_EQ(labelAtBoot(ResetKind::SoftwareRestart, TimePrecision::Exact, 1000, kOldAnchor), TimePrecision::Unknown);
  EXPECT_EQ(labelAtBoot(ResetKind::PowerOn, TimePrecision::Exact, 1000, kOldAnchor), TimePrecision::Unknown);
  // A power-on reset cannot be exact even if the bits look right.
  EXPECT_NE(labelAtBoot(ResetKind::PowerOn, TimePrecision::Exact, kNow, kOldAnchor), TimePrecision::Exact);
}

TEST(ClockRule6, BehindTheAnchorIsUnknown) {
  EXPECT_EQ(labelAtBoot(ResetKind::DeepSleepWake, TimePrecision::Unknown, kOldAnchor - 1, kOldAnchor),
            TimePrecision::Unknown);
  EXPECT_EQ(labelAtBoot(ResetKind::DeepSleepWake, TimePrecision::Unknown, kNow, 0), TimePrecision::Unknown);
}

TEST(ClockRule6, ApproximateStaysApproximateAcrossARestart) {
  EXPECT_EQ(labelAtBoot(ResetKind::SoftwareRestart, TimePrecision::Approximate, kNow, 0), TimePrecision::Approximate);
}

TEST(ClockRule6, AnUnauthenticatedTimeIsOnlyEverApproximate) {
  for (const ClockSource source : {ClockSource::Sntp, ClockSource::HttpDate, ClockSource::HttpsDate}) {
    FakeClock clock;
    clock.setWall(0, TimePrecision::Unknown);
    FakeCancel cancel;
    class One : public IClockSources {
     public:
      explicit One(ClockSource s) : s_(s) {}
      bool read(const ClockSource source, uint32_t, ICancel&, int64_t& out) override {
        if (source != s_) return false;
        out = kNow;
        return true;
      }
      ClockSource s_;
    } sources(source);
    ASSERT_TRUE(repairClock(clock, ClockTrust{}, 60000, sources, cancel));
    EXPECT_EQ(clock.wallNow().precision, TimePrecision::Approximate);
  }
}

TEST(ClockRule6, ARepairedTimeCannotRewindOrLeapTheClock) {
  ClockTrust trust;
  trust.anchorEpoch = kOldAnchor;
  trust.buildEpoch = kOldAnchor - 86400;
  EXPECT_TRUE(repairAcceptable(kNow, trust));
  EXPECT_TRUE(repairAcceptable(kOldAnchor, trust));
  EXPECT_FALSE(repairAcceptable(kOldAnchor - 1, trust)) << "behind the anchor";
  EXPECT_TRUE(repairAcceptable(kOldAnchor + kMaxClockGapSec, trust));
  EXPECT_FALSE(repairAcceptable(kOldAnchor + kMaxClockGapSec + 1, trust)) << "more than a year past it";
  EXPECT_FALSE(repairAcceptable(1000000000, trust)) << "out of bounds";
  ClockTrust noAnchor;
  noAnchor.buildEpoch = kNow;
  EXPECT_FALSE(repairAcceptable(kNow - 1, noAnchor)) << "before this firmware was built";
  EXPECT_TRUE(repairAcceptable(kNow + 86400, noAnchor));
}

TEST(ClockRule6, APersonCanBringBackAReaderLeftInADrawer) {
  ClockTrust trust;
  trust.anchorEpoch = kOldAnchor;
  trust.allowLongGap = true;
  EXPECT_TRUE(repairAcceptable(kOldAnchor + 3 * kMaxClockGapSec, trust));
  EXPECT_FALSE(repairAcceptable(kOldAnchor - 1, trust)) << "still never backwards";
}

TEST(ClockRule6, BuildDateParsesTheCompilersFormat) {
  EXPECT_EQ(epochOfBuildDate("Nov  5 2026"), 1793836800);
  EXPECT_EQ(epochOfBuildDate("Jan 01 2027"), 1798761600);
  EXPECT_EQ(epochOfBuildDate("nonsense"), 0);
  EXPECT_EQ(epochOfBuildDate(nullptr), 0);
  EXPECT_GT(epochOfBuildDate(__DATE__), 0) << __DATE__;
}

namespace {

class ScriptedSources : public IClockSources {
 public:
  explicit ScriptedSources(FakeClock& clock) : clock_(clock) {}
  std::vector<ClockSource> asked;
  std::vector<uint32_t> budgets;
  bool sntpOk = false, httpOk = false, httpsOk = false;
  int64_t epoch = kNow;
  uint32_t costMs = 0;
  bool read(const ClockSource source, const uint32_t budgetMs, ICancel&, int64_t& out) override {
    asked.push_back(source);
    budgets.push_back(budgetMs);
    clock_.mono += costMs < budgetMs ? costMs : budgetMs;
    const bool ok = source == ClockSource::Sntp ? sntpOk : source == ClockSource::HttpDate ? httpOk : httpsOk;
    if (ok) out = epoch;
    return ok;
  }

 private:
  FakeClock& clock_;
};

struct ClockFixture {
  FakeClock clock;
  FakeCancel cancel;
  ScriptedSources sources{clock};
};

}  // namespace

TEST(ClockChain, AValidClockAsksNothing) {
  ClockFixture f;
  f.clock.setWall(kNow, TimePrecision::Anchored);
  EXPECT_TRUE(ensureClock(f.clock, ClockTrust{}, 12000, f.sources, f.cancel));
  EXPECT_TRUE(f.sources.asked.empty());
}

TEST(ClockChain, AnchorComesBeforeTheNetwork) {
  ClockFixture f;
  f.clock.setWall(0, TimePrecision::Unknown);
  ClockTrust trust;
  trust.anchorEpoch = kOldAnchor;
  EXPECT_TRUE(ensureClock(f.clock, trust, 12000, f.sources, f.cancel));
  EXPECT_TRUE(f.sources.asked.empty());
  EXPECT_EQ(f.clock.wallNow().precision, TimePrecision::Anchored);
}

TEST(ClockChain, ShortSessionAsksTheCheapSourcesFirst) {
  ClockFixture f;
  f.clock.setWall(0, TimePrecision::Unknown);
  f.sources.httpsOk = true;
  EXPECT_TRUE(ensureClock(f.clock, ClockTrust{}, 12000, f.sources, f.cancel));
  ASSERT_EQ(f.sources.asked.size(), 2u);
  EXPECT_EQ(f.sources.asked[0], ClockSource::HttpDate);
  EXPECT_EQ(f.sources.asked[1], ClockSource::HttpsDate);
  EXPECT_EQ(f.clock.wallNow().precision, TimePrecision::Approximate);
}

TEST(ClockChain, RoomySessionAsksSntpFirst) {
  ClockFixture f;
  f.clock.setWall(0, TimePrecision::Unknown);
  f.sources.sntpOk = true;
  EXPECT_TRUE(ensureClock(f.clock, ClockTrust{}, 120000, f.sources, f.cancel));
  ASSERT_EQ(f.sources.asked.size(), 1u);
  EXPECT_EQ(f.sources.asked[0], ClockSource::Sntp);
  EXPECT_LE(f.sources.budgets[0], kSntpMaxMs);
  EXPECT_EQ(f.clock.wallNow().precision, TimePrecision::Approximate);
}

TEST(ClockChain, EveryStepStaysInsideTheBudget) {
  ClockFixture f;
  f.clock.setWall(0, TimePrecision::Unknown);
  f.sources.costMs = 100000;
  const uint64_t start = f.clock.monotonicMs();
  EXPECT_FALSE(ensureClock(f.clock, ClockTrust{}, 12000, f.sources, f.cancel));
  EXPECT_LE(f.clock.monotonicMs() - start, 12000u);
}

TEST(ClockChain, AnOutOfBoundsAnswerIsNotBelieved) {
  ClockFixture f;
  f.clock.setWall(0, TimePrecision::Unknown);
  f.sources.httpOk = true;
  f.sources.epoch = 1000000000;  // A portal stuck in 2001.
  EXPECT_FALSE(ensureClock(f.clock, ClockTrust{}, 12000, f.sources, f.cancel));
  EXPECT_EQ(f.clock.wallNow().precision, TimePrecision::Unknown);
}

TEST(ClockChain, RepairIgnoresTheAnchorAndAValidClock) {
  ClockFixture f;
  f.clock.setWall(kOldAnchor, TimePrecision::Anchored);
  f.sources.httpOk = true;
  ClockTrust trust;
  trust.anchorEpoch = kOldAnchor;
  EXPECT_TRUE(repairClock(f.clock, trust, 5000, f.sources, f.cancel));
  EXPECT_EQ(f.sources.asked.size(), 1u);
  EXPECT_EQ(f.clock.wallNow().epochSec, kNow);
}

TEST(ClockChain, CancelStopsTheChain) {
  ClockFixture f;
  f.clock.setWall(0, TimePrecision::Unknown);
  f.cancel.flag = true;
  EXPECT_FALSE(ensureClock(f.clock, ClockTrust{}, 12000, f.sources, f.cancel));
  EXPECT_TRUE(f.sources.asked.empty());
}

// --- Failure words (rules 7 and 12) ------------------------------------------------

TEST(NetFailureRule12, EachFailureHasItsPlainWord) {
  const auto word = [](TransportStage s, int32_t c, TimePrecision p = TimePrecision::Anchored) {
    return std::string(failureCode(s, c, p));
  };
  EXPECT_EQ(failureCode(TransportStage::None, 0, TimePrecision::Exact), nullptr);
  EXPECT_EQ(word(TransportStage::Dns, kCodeNoAddress), "dns_failed");
  EXPECT_EQ(word(TransportStage::Connect, kCodeTcp), "tcp_failed");
  EXPECT_EQ(word(TransportStage::Connect, kCodeLowMemory), "low_memory");
  EXPECT_EQ(word(TransportStage::Tls, wolfcode::kNoSigner), "tls_untrusted");
  EXPECT_EQ(word(TransportStage::Tls, wolfcode::kNameMismatch), "tls_name");
  EXPECT_EQ(word(TransportStage::Tls, wolfcode::kMemory), "low_memory");
  EXPECT_EQ(word(TransportStage::Tls, wolfcode::kMathMemory), "low_memory");
  EXPECT_EQ(word(TransportStage::Tls, wolfcode::kHashType), "tls_unsupported");
  EXPECT_EQ(word(TransportStage::Tls, kCodeAnchorLoad), "tls_anchor");
  EXPECT_EQ(word(TransportStage::Tls, kCodeTimeout), "tls_timeout");
  EXPECT_EQ(word(TransportStage::Tls, -313), "tls_failed");
  EXPECT_EQ(word(TransportStage::ReadBody, kCodeLost), "connection_lost");
  EXPECT_EQ(word(TransportStage::Aborted, 0), "aborted");
}

TEST(NetFailureRule12, CertificateDatesBlameTheClockOnlyWhenItCouldBeWrong) {
  EXPECT_STREQ(failureCode(TransportStage::Tls, wolfcode::kBeforeDate, TimePrecision::Anchored), "tls_clock");
  EXPECT_STREQ(failureCode(TransportStage::Tls, wolfcode::kAfterDate, TimePrecision::Unknown), "tls_clock");
  EXPECT_STREQ(failureCode(TransportStage::Tls, wolfcode::kAfterDate, TimePrecision::Exact), "tls_untrusted");
  EXPECT_TRUE(isClockFailure(TransportStage::Tls, wolfcode::kBeforeDate));
  EXPECT_FALSE(isClockFailure(TransportStage::Tls, wolfcode::kNoSigner));
}

TEST(NetFailureRule12, HeapGate) {
  EXPECT_TRUE(heapAllowsHandshake(50 * 1024, 20 * 1024));
  EXPECT_FALSE(heapAllowsHandshake(50 * 1024 - 1, 20 * 1024));
  EXPECT_FALSE(heapAllowsHandshake(200 * 1024, 20 * 1024 - 1));
}

// --- Records -----------------------------------------------------------------------

TEST(NetRecords, RoundTripAndSurviveTheirOwnNewerWrite) {
  MemFs fs;
  LinkHint hint;
  strcpy(hint.ssid, "Casa");
  const uint8_t bssid[6] = {1, 2, 3, 4, 5, 6};
  memcpy(hint.bssid, bssid, 6);
  hint.channel = 11;
  ASSERT_TRUE(saveLinkHint(fs, hint));
  LinkHint read;
  ASSERT_TRUE(loadLinkHint(fs, read));
  EXPECT_STREQ(read.ssid, "Casa");
  EXPECT_EQ(memcmp(read.bssid, bssid, 6), 0);
  EXPECT_EQ(read.channel, 11);
  EXPECT_TRUE(read.usable());

  strcpy(hint.ssid, "Oficina");
  ASSERT_TRUE(saveLinkHint(fs, hint));
  ASSERT_TRUE(loadLinkHint(fs, read));
  EXPECT_STREQ(read.ssid, "Oficina");

  DnsMemory memory;
  strcpy(memory.host, kProductionHost);
  memory.ip[0] = 104;
  ASSERT_TRUE(saveDnsMemory(fs, memory));
  DnsMemory m2;
  ASSERT_TRUE(loadDnsMemory(fs, m2));
  EXPECT_STREQ(m2.host, kProductionHost);
  EXPECT_EQ(m2.ip[0], 104);

  NetDiag diag;
  diag.stage = TransportStage::Tls;
  diag.code = -188;
  diag.atEpoch = kNow;
  strcpy(diag.failure, "tls_untrusted");
  ASSERT_TRUE(saveNetDiag(fs, diag));
  NetDiag d2;
  ASSERT_TRUE(loadNetDiag(fs, d2));
  EXPECT_EQ(d2.stage, TransportStage::Tls);
  EXPECT_EQ(d2.code, -188);
  EXPECT_EQ(d2.atEpoch, kNow);
  EXPECT_STREQ(d2.failure, "tls_untrusted");
}

TEST(NetRecords, MissingFilesAreJustMissing) {
  MemFs fs;
  LinkHint hint;
  EXPECT_FALSE(loadLinkHint(fs, hint));
  DnsMemory memory;
  EXPECT_FALSE(loadDnsMemory(fs, memory));
  NetDiag diag;
  EXPECT_FALSE(loadNetDiag(fs, diag));
}
