#include "JsonStream.h"
#include "TestSupport.h"

using namespace borges;
using namespace borges_test;

namespace {

class StringSource : public IByteSource {
 public:
  explicit StringSource(std::string text) : text_(std::move(text)) {}
  int32_t read(uint8_t* dst, size_t len) override {
    if (pos_ >= text_.size()) return 0;
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

bool structurallyValid(const std::string& text) {
  StringSource source(text);
  JsonScanner scan(source);
  return scan.skipValue() && scan.atEnd();
}

}  // namespace

TEST(JsonStream, WalksKnownFieldsAndSkipsTheRest) {
  StringSource source(R"( { "a" : "text", "ignored": {"x":[1,2,{"y":null}]}, "n": 42, "b": true, "arr": ["p", "q"] } )");
  JsonScanner scan(source);
  ASSERT_TRUE(scan.enterObject());
  char key[16];
  char text[16];
  int64_t n = 0;
  bool b = false;
  std::vector<std::string> arr;
  while (scan.nextMember(key, sizeof(key))) {
    if (!strcmp(key, "a")) {
      ASSERT_TRUE(scan.readString(text, sizeof(text)));
    } else if (!strcmp(key, "n")) {
      ASSERT_TRUE(scan.readInt(n));
    } else if (!strcmp(key, "b")) {
      ASSERT_TRUE(scan.readBool(b));
    } else if (!strcmp(key, "arr")) {
      ASSERT_TRUE(scan.enterArray());
      while (scan.nextElement()) {
        char item[4];
        ASSERT_TRUE(scan.readString(item, sizeof(item)));
        arr.push_back(item);
      }
    } else {
      ASSERT_TRUE(scan.skipValue());
    }
  }
  EXPECT_FALSE(scan.failed());
  EXPECT_TRUE(scan.atEnd());
  EXPECT_STREQ(text, "text");
  EXPECT_EQ(n, 42);
  EXPECT_TRUE(b);
  EXPECT_EQ(arr, (std::vector<std::string>{"p", "q"}));
}

TEST(JsonStream, DecodesEscapesAndSurrogates) {
  StringSource source(R"("a\n\"\\\/é😀\ud83dx\u0000")");
  JsonScanner scan(source);
  char out[40];
  ASSERT_TRUE(scan.readString(out, sizeof(out)));
  // Lone surrogate and NUL become U+FFFD.
  EXPECT_STREQ(out, "a\n\"\\/\xC3\xA9\xF0\x9F\x98\x80\xEF\xBF\xBDx\xEF\xBF\xBD");
}

TEST(JsonStream, TruncationIsFlaggedAndNeverSplitsASequence) {
  StringSource source("\"abc\xC3\xA9xyz\"");
  JsonScanner scan(source);
  char out[5];
  bool cut = false;
  ASSERT_TRUE(scan.readString(out, sizeof(out), &cut));
  EXPECT_TRUE(cut);
  EXPECT_STREQ(out, "abc");
}

TEST(JsonStream, InvalidUtf8NeverReachesATitleOrANote) {
  const auto read = [](const std::string& raw) {
    StringSource source("\"" + raw + "\"");
    JsonScanner scan(source);
    char out[32];
    EXPECT_TRUE(scan.readString(out, sizeof(out)));
    return std::string(out);
  };
  const std::string fffd = "\xEF\xBF\xBD";
  EXPECT_EQ(read("a\xFFz"), "a" + fffd + "z");                         // Never a lead byte.
  EXPECT_EQ(read("a\xC3z"), "a" + fffd + "z");                         // Lead with no continuation.
  EXPECT_EQ(read("\xC0\xAF"), fffd + fffd);                            // Overlong slash.
  EXPECT_EQ(read("\xED\xA0\x80"), fffd + fffd + fffd);                 // Encoded surrogate.
  EXPECT_EQ(read("\xF4\x90\x80\x80"), fffd + fffd + fffd + fffd);      // Above U+10FFFF.
  EXPECT_EQ(read("\xE2\x82"), fffd);                                   // Cut by the closing quote.
  EXPECT_EQ(read("\xC3\xA9\xF0\x9F\x98\x80"), "\xC3\xA9\xF0\x9F\x98\x80");  // Real text is untouched.
}

TEST(JsonStream, ControlCharactersNeverReachATitleOrANote) {
  StringSource source(R"("a\u0001b\u001fc\u007fd\be\ff\rg\nh\ti")");
  JsonScanner scan(source);
  char out[40];
  ASSERT_TRUE(scan.readString(out, sizeof(out)));
  EXPECT_STREQ(out, "a b c d e f g\nh\ti");
}

TEST(JsonStream, TypeMismatchConsumesWithoutFailing) {
  StringSource source(R"({"a":{"deep":[1]},"b":"text","c":[1,2],"d":"7"})");
  JsonScanner scan(source);
  ASSERT_TRUE(scan.enterObject());
  char key[8];
  char text[8];
  int64_t n = 0;
  bool b = false;
  uint64_t seq = 0;
  ASSERT_TRUE(scan.nextMember(key, sizeof(key)));
  EXPECT_FALSE(scan.readString(text, sizeof(text)));
  ASSERT_TRUE(scan.nextMember(key, sizeof(key)));
  EXPECT_FALSE(scan.readInt(n));
  ASSERT_TRUE(scan.nextMember(key, sizeof(key)));
  EXPECT_FALSE(scan.readBool(b));
  ASSERT_TRUE(scan.nextMember(key, sizeof(key)));
  EXPECT_TRUE(scan.readSequence(seq));
  EXPECT_EQ(seq, 7u);
  EXPECT_FALSE(scan.nextMember(key, sizeof(key)));
  EXPECT_FALSE(scan.failed());
}

TEST(JsonStream, PercentIsExactAndBounded) {
  const auto parse = [](const std::string& text, uint32_t& out) {
    StringSource source(text);
    JsonScanner scan(source);
    return scan.readPercentMicro(out);
  };
  uint32_t v = 0;
  ASSERT_TRUE(parse("60", v));
  EXPECT_EQ(v, 60000000u);
  ASSERT_TRUE(parse("12.5", v));
  EXPECT_EQ(v, 12500000u);
  ASSERT_TRUE(parse("0.000001", v));
  EXPECT_EQ(v, 1u);
  ASSERT_TRUE(parse("100", v));
  EXPECT_EQ(v, 100000000u);
  ASSERT_TRUE(parse("1.25e1", v));
  EXPECT_EQ(v, 12500000u);
  ASSERT_TRUE(parse("1e-7", v));
  EXPECT_EQ(v, 0u);
  ASSERT_TRUE(parse("5e-7", v));
  EXPECT_EQ(v, 1u);
  ASSERT_TRUE(parse("99.99999999", v));
  EXPECT_EQ(v, 100000000u);
  // The digit that rounds is the most significant one dropped, however far down it is.
  ASSERT_TRUE(parse("0.0000000099", v));
  EXPECT_EQ(v, 0u);
  ASSERT_TRUE(parse("0.00000149", v));
  EXPECT_EQ(v, 1u);
  ASSERT_TRUE(parse("9e-9", v));
  EXPECT_EQ(v, 0u);
  EXPECT_FALSE(parse("100.000001", v));
  EXPECT_FALSE(parse("-1", v));
  EXPECT_FALSE(parse("1e400", v));
  EXPECT_FALSE(parse("123456789012345678901234567890", v));
}

TEST(JsonStream, SkipValidatesStructure) {
  EXPECT_TRUE(structurallyValid(R"({"a":[1,2,{"b":null}],"c":"d","e":-1.5e3,"f":true,"g":false})"));
  EXPECT_TRUE(structurallyValid("[]"));
  EXPECT_TRUE(structurallyValid("{}"));
  EXPECT_TRUE(structurallyValid("  null "));
  const char* bad[] = {"",
                       "{",
                       "[",
                       "{\"a\"}",
                       "{\"a\":}",
                       "{\"a\":1,}",
                       "[1,]",
                       "[,1]",
                       "{1:2}",
                       "[1 2]",
                       "{\"a\":1]",
                       "[1}",
                       "\"unterminated",
                       "\"bad\\q\"",
                       "\"bad\\u12\"",
                       "\"control\x01\"",
                       "tru",
                       "nul",
                       "01",
                       "1.",
                       "1e",
                       "-",
                       "{} {}",
                       "[1]]"};
  for (const char* text : bad) EXPECT_FALSE(structurallyValid(text)) << text;
}

TEST(JsonStream, HugeNestingCostsOnlyACounter) {
  const size_t depth = 200000;
  std::string deep(depth, '[');
  deep += std::string(depth, ']');
  EXPECT_TRUE(structurallyValid(deep));
  EXPECT_FALSE(structurallyValid(std::string(depth, '[')));

  std::string objects;
  for (int i = 0; i < 1000; ++i) objects += "{\"k\":";
  objects += "1";
  for (int i = 0; i < 1000; ++i) objects += "}";
  EXPECT_TRUE(structurallyValid(objects));
}

TEST(JsonStream, OffsetsAllowComingBack) {
  const std::string text = R"({"x":1,"events":[{"a":1},{"a":2}],"cursor":"9"})";
  StringSource source(text);
  JsonScanner scan(source);
  ASSERT_TRUE(scan.enterObject());
  char key[12];
  uint32_t eventsAt = 0;
  uint64_t cursor = 0;
  while (scan.nextMember(key, sizeof(key))) {
    if (!strcmp(key, "events")) {
      ASSERT_EQ(scan.peek(), JsonType::Array);
      eventsAt = scan.offset();
      scan.skipValue();
    } else if (!strcmp(key, "cursor")) {
      scan.readSequence(cursor);
    } else {
      scan.skipValue();
    }
  }
  EXPECT_EQ(cursor, 9u);
  EXPECT_EQ(text[eventsAt], '[');
  scan.seekTo(eventsAt);
  ASSERT_TRUE(scan.enterArray());
  int count = 0;
  while (scan.nextElement()) {
    ++count;
    scan.skipValue();
  }
  EXPECT_EQ(count, 2);
  EXPECT_FALSE(scan.failed());
}

TEST(JsonStream, ValuesCrossingTheWindowParse) {
  std::string big(1000, 'z');
  StringSource source("{\"pad\":\"" + big + "\",\"name\":\"" + std::string(180, 'y') + "\",\"n\":123456}");
  JsonScanner scan(source);
  ASSERT_TRUE(scan.enterObject());
  char key[8];
  char name[200];
  int64_t n = 0;
  while (scan.nextMember(key, sizeof(key))) {
    if (!strcmp(key, "name")) {
      ASSERT_TRUE(scan.readString(name, sizeof(name)));
    } else if (!strcmp(key, "n")) {
      ASSERT_TRUE(scan.readInt(n));
    } else {
      scan.skipValue();
    }
  }
  EXPECT_EQ(strlen(name), 180u);
  EXPECT_EQ(n, 123456);
}
