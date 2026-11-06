#include "Backoff.h"
#include "Codec.h"
#include "TestSupport.h"

using namespace borges;
using namespace borges_test;

TEST(Codec, Crc32MatchesKnownVector) { EXPECT_EQ(crc32("123456789", 9), 0xCBF43926u); }

TEST(Codec, Md5MatchesRfc1321Vectors) {
  const auto digest = [](const std::string& text) {
    Md5 md5;
    md5.update(text.data(), text.size());
    uint8_t out[16];
    md5.finish(out);
    char hex[33];
    hexEncode(out, 16, hex);
    return std::string(hex);
  };
  EXPECT_EQ(digest(""), "d41d8cd98f00b204e9800998ecf8427e");
  EXPECT_EQ(digest("abc"), "900150983cd24fb0d6963f7d28e17f72");
  EXPECT_EQ(digest("12345678901234567890123456789012345678901234567890123456789012345678901234567890"),
            "57edf4a22be3c955ac49da2e2107b67a");
}

TEST(Codec, Md5OfPiecesEqualsMd5OfWhole) {
  std::string data(1000, 'q');
  Md5 whole;
  whole.update(data.data(), data.size());
  Md5 pieces;
  for (size_t i = 0; i < data.size(); i += 7) pieces.update(data.data() + i, std::min<size_t>(7, data.size() - i));
  uint8_t a[16], b[16];
  whole.finish(a);
  pieces.finish(b);
  EXPECT_EQ(memcmp(a, b, 16), 0);
}

TEST(Codec, ParseSequenceFollowsServerGrammar) {
  uint64_t v = 0;
  EXPECT_TRUE(parseSequence("0", v));
  EXPECT_EQ(v, 0u);
  EXPECT_TRUE(parseSequence("9223372036854775807", v));
  EXPECT_EQ(v, 9223372036854775807ULL);
  EXPECT_FALSE(parseSequence("9223372036854775808", v));
  EXPECT_FALSE(parseSequence("007", v));
  EXPECT_FALSE(parseSequence("", v));
  EXPECT_FALSE(parseSequence("-1", v));
  EXPECT_FALSE(parseSequence("1.5", v));
  EXPECT_FALSE(parseSequence(nullptr, v));
}

TEST(Codec, UuidRoundTripAndServerShape) {
  uint8_t raw[16];
  ASSERT_TRUE(uuidParse("20000000-0000-4000-8000-000000000004", raw));
  char text[37];
  uuidFormat(raw, text);
  EXPECT_STREQ(text, "20000000-0000-4000-8000-000000000004");
  EXPECT_TRUE(isCanonicalUuid(text));
  EXPECT_FALSE(isCanonicalUuid("20000000-0000-0000-8000-000000000004"));  // version 0
  EXPECT_FALSE(isCanonicalUuid("20000000-0000-4000-c000-000000000004"));  // variant
  EXPECT_FALSE(isCanonicalUuid("not-a-uuid"));
  FakeRandom random;
  uuidV4(random, raw);
  uuidFormat(raw, text);
  EXPECT_TRUE(isCanonicalUuid(text));
}

TEST(Codec, UuidFromSeedIsStableAndValid) {
  uint8_t a[16], b[16];
  uuidFromSeed("seed", 4, a);
  uuidFromSeed("seed", 4, b);
  EXPECT_EQ(memcmp(a, b, 16), 0);
  char text[37];
  uuidFormat(a, text);
  EXPECT_TRUE(isCanonicalUuid(text));
}

TEST(Codec, Rfc3339RoundTrip) {
  int64_t epoch = 0;
  ASSERT_TRUE(parseRfc3339("2026-07-24T08:00:01.000Z", epoch));
  EXPECT_EQ(epoch, 1784880001);
  char text[21];
  formatRfc3339(epoch, text);
  EXPECT_STREQ(text, "2026-07-24T08:00:01Z");
  ASSERT_TRUE(parseRfc3339("2026-07-24T05:00:01-03:00", epoch));
  EXPECT_EQ(epoch, 1784880001);
  ASSERT_TRUE(parseRfc3339("2000-02-29T00:00:00+00:00", epoch));
  formatRfc3339(epoch, text);
  EXPECT_STREQ(text, "2000-02-29T00:00:00Z");
  EXPECT_FALSE(parseRfc3339("2026-13-01T00:00:00Z", epoch));
  EXPECT_FALSE(parseRfc3339("2026-07-24 08:00:01Z", epoch));
  EXPECT_FALSE(parseRfc3339("2026-07-24T08:00:01", epoch));
  EXPECT_FALSE(parseRfc3339("2026-07-24T08:00:01.Z", epoch));
  EXPECT_FALSE(parseRfc3339("", epoch));
}

TEST(Codec, Base64) {
  uint8_t out[16];
  EXPECT_EQ(base64Decode("aGVsbG8=", out, sizeof(out)), 5);
  EXPECT_EQ(memcmp(out, "hello", 5), 0);
  EXPECT_EQ(base64Decode("aGVsbG8", out, sizeof(out)), 5);
  EXPECT_EQ(base64Decode("a$b", out, sizeof(out)), -1);
  EXPECT_EQ(base64Decode("aGVsbG8=", out, 2), -1);
  const uint8_t raw[3] = {0xFB, 0xEF, 0xBE};
  char text[8];
  base64UrlEncode(raw, 3, text);
  EXPECT_STREQ(text, "----");
}

TEST(Codec, Utf8CopyNeverSplitsASequence) {
  char out[5];
  EXPECT_EQ(utf8Copy(out, sizeof(out), "ab\xC3\xA9xx", 6), 4u);
  EXPECT_STREQ(out, "ab\xC3\xA9");
  EXPECT_EQ(utf8Copy(out, sizeof(out), "abc\xC3\xA9x", 6), 3u);
  EXPECT_STREQ(out, "abc");
}

TEST(Codec, JsonWriterEscapesAndCounts) {
  const auto write = [](JsonWriter& w) {
    w.objectBegin();
    w.kv("a\"b", "line\n\"q\"\\\x01");
    w.kvSeq("seq", 42);
    w.kvBool("t", true);
    w.kvNull("n");
    w.key("arr");
    w.arrayBegin();
    w.valueInt(-3);
    w.valuePercent(12500000);
    w.valuePercent(100000000);
    w.valuePercent(1);
    w.arrayEnd();
    w.objectEnd();
  };
  JsonWriter counter(nullptr, 0);
  write(counter);
  std::string buffer(counter.size(), '\0');
  JsonWriter w(buffer.data(), buffer.size());
  write(w);
  ASSERT_TRUE(w.ok());
  EXPECT_EQ(w.size(), counter.size());
  EXPECT_EQ(buffer,
            "{\"a\\\"b\":\"line\\n\\\"q\\\"\\\\\\u0001\",\"seq\":\"42\",\"t\":true,\"n\":null,\"arr\":[-3,12.5,100,0."
            "000001]}");
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, buffer));
  EXPECT_EQ(doc["seq"], "42");
}

TEST(Codec, JsonWriterOverflowIsReportedNotWritten) {
  char small[4] = {'x', 'x', 'x', 'x'};
  JsonWriter w(small, 3);
  w.value("abcdef");
  EXPECT_FALSE(w.ok());
  EXPECT_EQ(small[3], 'x');
}

TEST(Backoff, FollowsTheScheduleWithBoundedJitter) {
  FakeRandom random;
  for (uint8_t n = 0; n < 40; ++n) {
    uint64_t base = n < 16 ? (15ULL << n) : backoff::kCapSec;
    if (base > backoff::kCapSec) base = backoff::kCapSec;
    for (int i = 0; i < 50; ++i) {
      const uint32_t d = backoff::delaySec(n, random);
      EXPECT_GE(d, base * 8 / 10) << "n=" << static_cast<int>(n);
      EXPECT_LE(d, std::min<uint64_t>(base * 12 / 10, backoff::kCapSec)) << "n=" << static_cast<int>(n);
    }
  }
}

TEST(Backoff, RetryAfterOnlyLengthens) {
  const WallTime now{kNow, TimePrecision::Exact};
  EXPECT_EQ(backoff::nextAttempt(now, 15, true, 120), kNow + 120);
  EXPECT_EQ(backoff::nextAttempt(now, 300, true, 120), kNow + 300);
  EXPECT_EQ(backoff::nextAttempt(now, 300, false, 0), kNow + 300);
}

TEST(Backoff, UnknownClockNeverBlocks) {
  const WallTime unknown{kNow, TimePrecision::Unknown};
  EXPECT_EQ(backoff::nextAttempt(unknown, 300, false, 0), 0);
  EXPECT_FALSE(backoff::blocked(unknown, kNow + 1000));
  const WallTime exact{kNow, TimePrecision::Exact};
  EXPECT_TRUE(backoff::blocked(exact, kNow + 1000));
  EXPECT_FALSE(backoff::blocked(exact, kNow));
  EXPECT_FALSE(backoff::blocked(exact, 0));
}
