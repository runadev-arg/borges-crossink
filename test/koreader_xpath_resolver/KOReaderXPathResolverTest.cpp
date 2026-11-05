#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ChapterXPathResolver.h"

namespace {
std::shared_ptr<Epub> epubWith(std::string xhtml) {
  std::vector<std::string> spine;
  spine.push_back(std::move(xhtml));
  return std::make_shared<Epub>(std::move(spine));
}

constexpr char kNestedFixture[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><div><section><p>Alpha bravo</p><p>Second <em>nested</em> tail</p></section></div></body></html>)";
constexpr char kCommentBoundaryFixture[] = R"(<html><body><p>before<!--comment-->after</p></body></html>)";
constexpr char kProcessingInstructionBoundaryFixture[] = R"(<html><body><p>before<?marker?>after</p></body></html>)";
constexpr char kCdataBoundaryFixture[] = R"(<html><body><p>before<![CDATA[middle]]>after</p></body></html>)";
constexpr char kHiddenCdataFixture[] = R"(<html><body><p>before<rp><![CDATA[hidden]]></rp>after</p></body></html>)";
}  // namespace

// Every string these tests expect is what crengine itself wrote for that position: see
// test/borges_xpointer. A document with one spine item has a DocFragment with no index.
TEST(KOReaderXPathResolver, ResolvesExactOffsetWithFullAncestry) {
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith(kNestedFixture), 0, 6),
            "/body/DocFragment/body/div/section/p[1]/text().6");
}

TEST(KOReaderXPathResolver, PreservesNestedInlineTextNode) {
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith(kNestedFixture), 0, 26),
            "/body/DocFragment/body/div/section/p[2]/text()[2].2");
}

TEST(KOReaderXPathResolver, EmitsDetailedAnchorForOffsetZero) {
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith(kNestedFixture), 0, 0),
            "/body/DocFragment/body/div/section/p[1]/text().0");
}

TEST(KOReaderXPathResolver, IgnoresNestedNonVisibleInlineText) {
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(
                epubWith("<html><body><p><rp><span>hidden</span></rp>Visible text</p></body></html>"), 0, 0),
            "/body/DocFragment/body/p/text().0");
}

TEST(KOReaderXPathResolver, ResolvesProgressAfterNestedNonVisibleInlineText) {
  EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(
                epubWith("<html><body><p><rp><span>hidden</span></rp>Visible text</p></body></html>"), 0, 1.0f),
            "/body/DocFragment/body/p/text().11");
}

TEST(KOReaderXPathResolver, CountsUtf8CodepointsInsteadOfBytes) {
  const auto epub = epubWith(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><html><body><p>A\xC3\xA9\xE4\xB8\xAD"
      "B</p></body></html>");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 3), "/body/DocFragment/body/p/text().3");
}

TEST(KOReaderXPathResolver, SplitsTextNodesAroundComments) {
  const auto epub = epubWith(kCommentBoundaryFixture);
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 6), "/body/DocFragment/body/p/text()[2].0");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 7), "/body/DocFragment/body/p/text()[2].1");
  // At the very end, the last character; past it, nothing.
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 11), "/body/DocFragment/body/p/text()[2].4");
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 12).empty());
}

TEST(KOReaderXPathResolver, SplitsTextNodesAroundProcessingInstructions) {
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith(kProcessingInstructionBoundaryFixture), 0, 7),
            "/body/DocFragment/body/p/text()[2].1");
}

TEST(KOReaderXPathResolver, SplitsTextNodesAroundCdata) {
  const auto epub = epubWith(kCdataBoundaryFixture);
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 7), "/body/DocFragment/body/p/text()[2].1");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 13), "/body/DocFragment/body/p/text()[3].1");
}

TEST(KOReaderXPathResolver, CountsVisibleCdataAndIgnoresHiddenCdata) {
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith(kHiddenCdataFixture), 0, 7),
            "/body/DocFragment/body/p/text()[2].1");
}

TEST(KOReaderXPathResolver, TextDirectlyInBodyIsItsOwnNode) {
  EXPECT_EQ(
      ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith("<html><body>Direct text</body></html>"), 0, 0),
      "/body/DocFragment/body/text().0");
}

TEST(KOReaderXPathResolver, ReturnsEmptyForUnusableContent) {
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith(""), 0, 0).empty());
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith("<html><body><p>broken"), 0, 100).empty());
}

TEST(KOReaderXPathResolver, KeepsParagraphOnlyResolutionUnchanged) {
  EXPECT_EQ(ChapterXPathResolver::findXPathForParagraph(epubWith(kNestedFixture), 0, 2),
            "/body/DocFragment/body/div/section/p[2]");
}

TEST(KOReaderXPathResolver, NumbersDocFragmentsWhenThereIsMoreThanOne) {
  const auto epub = std::make_shared<Epub>(
      std::vector<std::string>{"<html><body><p>a</p></body></html>", "<html><body><p>b</p></body></html>"});
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 1, 0), "/body/DocFragment[2]/body/p/text().0");
  EXPECT_EQ(ChapterXPathResolver::bodyXPath(epub, 1), "/body/DocFragment[2]/body");
}

namespace {
// Inky keeps formatting whitespace around a split chapter's mapped children.
constexpr char kSplitFixture[] =
    "<html><body>\n\n <section>\n\n <p>Alpha</p>\n\n <p>Bravo</p>\n\n </section>\n</body></html>";
}  // namespace

TEST(KOReaderXPathResolver, SplitStartSkipsBodyAndContainerWhitespace) {
  const auto epub = epubWith(kSplitFixture);
  for (uint32_t offset = 0; offset <= 6; ++offset) {
    EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, offset, 2),
              "/body/DocFragment/body/section/p[1]/text().0")
        << offset;
  }
  EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, 0.0f, 2),
            "/body/DocFragment/body/section/p[1]/text().0");
}

TEST(KOReaderXPathResolver, SplitInteriorRetainsExactOffset) {
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith(kSplitFixture), 0, 8, 2),
            "/body/DocFragment/body/section/p[1]/text().2");
}

TEST(KOReaderXPathResolver, SplitGapAdvancesToNextChild) {
  const auto epub = epubWith(kSplitFixture);
  for (uint32_t offset = 11; offset <= 14; ++offset) {
    EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, offset, 2),
              "/body/DocFragment/body/section/p[2]/text().0")
        << offset;
  }
}

TEST(KOReaderXPathResolver, SplitEndUsesLastContentCharacter) {
  const auto epub = epubWith(kSplitFixture);
  for (uint32_t offset = 19; offset <= 23; ++offset) {
    EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, offset, 2),
              "/body/DocFragment/body/section/p[2]/text().4")
        << offset;
  }
  EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, 1.0f, 2),
            "/body/DocFragment/body/section/p[2]/text().4");
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 100, 2).empty());
}

TEST(KOReaderXPathResolver, SplitWithoutMappedContentDoesNotInventAnchor) {
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(
                  epubWith("<html><body>\n<section>\n</section></body></html>"), 0, 0, 2)
                  .empty());
}

TEST(KOReaderXPathResolver, SplitDirectChildrenAndNestedInlineText) {
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(
                epubWith("<html><body>\n<p><em>Hello</em></p></body></html>"), 0, 0, 1),
            "/body/DocFragment/body/p/em/text().0");
}

// ---- Regressions: positions crengine reads differently from a plain walk of the XHTML. ---------
namespace {
// The visible offset of the first occurrence of `needle` in a fixture's text.
uint32_t offsetOf(const std::string& body, const std::string& needle) {
  uint32_t visible = 0;
  bool inTag = false;
  for (size_t i = 0; i < body.size(); i++) {
    if (body[i] == '<') inTag = true;
    if (!inTag && body.compare(i, needle.size(), needle) == 0) return visible;
    if (!inTag && (static_cast<unsigned char>(body[i]) & 0xC0) != 0x80) visible++;
    if (body[i] == '>') inTag = false;
  }
  ADD_FAILURE() << needle;
  return 0;
}

void expectBothWays(const std::string& body, const std::string& word, const std::string& pointer) {
  const auto epub = epubWith("<html><body>" + body + "</body></html>");
  const uint32_t offset = offsetOf(body, word);
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, offset), pointer) << word;
  ChapterXPathResolver::Resolved resolved;
  ASSERT_TRUE(ChapterXPathResolver::resolve(epub, 0, pointer, resolved)) << pointer;
  EXPECT_EQ(resolved.visibleOffset, offset) << pointer;
  EXPECT_TRUE(resolved.exact);
}
}  // namespace

TEST(KOReaderXPathRegression, SiblingsAreCountedInTheirOwnParent) {
  const std::string body = "<p>one</p><blockquote><p>q1</p><p>q2</p></blockquote><p>two</p><p>three</p>";
  expectBothWays(body, "three", "/body/DocFragment/body/p[3]/text().0");
  expectBothWays(body, "q2", "/body/DocFragment/body/blockquote/p[2]/text().0");
  const std::string divs = "<div><div><p>inner</p></div><p>in-first</p></div><div><p>second-div</p></div>";
  expectBothWays(divs, "second", "/body/DocFragment/body/div[2]/p/text().0");
  expectBothWays(divs, "inner", "/body/DocFragment/body/div[1]/div/p/text().0");
}

TEST(KOReaderXPathRegression, AnInlineChildDoesNotStartATextNodeBeforeItIsFollowedByText) {
  const std::string anchored = "<p>pad</p><p><a id=\"x\"/>Hello world again</p>";
  expectBothWays(anchored, "world", "/body/DocFragment/body/p[2]/text().6");
  const std::string inlines = "<p><i>Foo</i> bar one <b>baz</b> qux two</p>";
  expectBothWays(inlines, "bar", "/body/DocFragment/body/p/text()[1].1");
  expectBothWays(inlines, "qux", "/body/DocFragment/body/p/text()[2].1");
  expectBothWays(inlines, "baz", "/body/DocFragment/body/p/b/text().0");
}

TEST(KOReaderXPathRegression, EmptyTextBeforeABlockIsNotANode) {
  expectBothWays("<div>\n<p>bbb</p></div>", "bbb", "/body/DocFragment/body/div/p/text().0");
  const auto epub = epubWith("<html><body><div>\n<p>bbb</p></div></body></html>");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 0), "/body/DocFragment/body/div/p/text().0");
  const std::string mixed = "<div>Lead <p>x</p> <p>y</p> tail <p>z</p>\n after <b>b</b> <i>i</i> end\n</div>";
  expectBothWays(mixed, "tail", "/body/DocFragment/body/div/text()[2].1");
  expectBothWays(mixed, "after", "/body/DocFragment/body/div/text()[3].1");
  // Between two inlines it stays, and counts.
  expectBothWays(mixed, "end", "/body/DocFragment/body/div/text()[5].1");
}

TEST(KOReaderXPathRegression, ABrIsInlineWhateverItsDisplaySays) {
  // Calibre output sets display: block on a class that <br> carries.
  const std::string body =
      "<h1>\nLead words <br style=\"display:block\"/>\n<br style=\"display:block\"/>\nSecond part "
      "<br style=\"display:block\"/>\n</h1>";
  expectBothWays(body, "Lead", "/body/DocFragment/body/h1/text()[1].1");
  expectBothWays(body, "Second", "/body/DocFragment/body/h1/text()[3].1");
}

TEST(KOReaderXPathRegression, APageStartingAtAnImageNamesTheTextAfterIt) {
  const std::string body = "<div><p>aaa</p>\n<img src=\"x\"/>\n<p>bbb</p></div>";
  const auto epub = epubWith("<html><body>" + body + "</body></html>");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 3),
            "/body/DocFragment/body/div/p[2]/text().0");
}

TEST(KOReaderXPathRegression, OffsetsCountCollapsedText) {
  const std::string wrapped = "<p>\n        Hello\n        world\n        again</p>";
  expectBothWays(wrapped, "world", "/body/DocFragment/body/p/text().7");
  expectBothWays("<div>Lead <span>inner</span> tail text</div>", "tail", "/body/DocFragment/body/div/text()[2].1");
  expectBothWays("<p>  two   spaces\there  </p>", "spaces", "/body/DocFragment/body/p/text().5");
  expectBothWays("<pre>x\ty\tz\n\tw</pre>", "w", "/body/DocFragment/body/pre/text().25");
  expectBothWays("<pre>\nfirst</pre>", "first", "/body/DocFragment/body/pre/text().0");
  expectBothWays("<p style=\"white-space: pre\">a  b</p>", "b", "/body/DocFragment/body/p/text().3");
}

TEST(KOReaderXPathRegression, EntitiesAreOneCharacterAndCommentsSplit) {
  const auto epub = epubWith(
      "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">"
      "<html><body><p>a&nbsp;b &amp; &#32;&#32;c&#x1F600;d</p></body></html>");
  ChapterXPathResolver::Resolved resolved;
  ASSERT_TRUE(ChapterXPathResolver::resolve(epub, 0, "/body/DocFragment/body/p/text().10", resolved));
  // a nbsp b space amp space sp sp c emoji d: stored positions and counted ones agree here.
  EXPECT_EQ(resolved.visibleOffset, 10u);
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 10), "/body/DocFragment/body/p/text().10");
  expectBothWays("<p>a<!-- c -->b<!-- d --> <!-- e -->c</p>", "c</p>", "/body/DocFragment/body/p/text()[4].0");
}

TEST(KOReaderXPathRegression, TablesDiscardTheirOwnText) {
  const std::string body = "<table>stray<tr>loose<td>cell</td>more</tr>tail</table>after";
  expectBothWays(body, "cell", "/body/DocFragment/body/table/tr/td/text().0");
  expectBothWays(body, "after", "/body/DocFragment/body/text().0");
}

TEST(KOReaderXPathRegression, TextNodesSplitAt8192SourceCharacters) {
  std::string words;
  for (int i = 0; i < 2000; i++) words += "word ";
  const std::string body = "<p>" + words + "</p>";
  const auto epub = epubWith("<html><body>" + body + "</body></html>");
  // crengine ends the first node at the last space of the first 8192 characters.
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 8185),
            "/body/DocFragment/body/p/text()[1].8185");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 8190), "/body/DocFragment/body/p/text()[2].0");
  ChapterXPathResolver::Resolved resolved;
  ASSERT_TRUE(ChapterXPathResolver::resolve(epub, 0, "/body/DocFragment/body/p/text()[2].5", resolved));
  EXPECT_EQ(resolved.visibleOffset, 8195u);
}

TEST(KOReaderXPathRegression, UnknownNodesFallBackAndUnknownElementsAreRefused) {
  const auto epub = epubWith("<html><body><p>abc</p><p>def</p></body></html>");
  ChapterXPathResolver::Resolved resolved;
  EXPECT_FALSE(ChapterXPathResolver::resolve(epub, 0, "/body/DocFragment/body/p[3]/text().0", resolved));
  EXPECT_FALSE(ChapterXPathResolver::resolve(epub, 0, "not a pointer", resolved));
  // The paragraph is there, not that character: its end is the nearest one.
  ASSERT_TRUE(ChapterXPathResolver::resolve(epub, 0, "/body/DocFragment/body/p[1]/text().99", resolved));
  EXPECT_EQ(resolved.visibleOffset, 3u);
  EXPECT_FALSE(resolved.exact);
}

TEST(KOReaderXPathRegression, ElementPointersLandOnTheTextThatFollows) {
  const auto epub = epubWith("<html><body><p>abc</p><div><img src=\"x\"/></div><p>def</p></body></html>");
  ChapterXPathResolver::Resolved resolved;
  for (const char* pointer : {"/body/DocFragment/body/div", "/body/DocFragment/body/div/img.0"}) {
    ASSERT_TRUE(ChapterXPathResolver::resolve(epub, 0, pointer, resolved)) << pointer;
    EXPECT_EQ(resolved.visibleOffset, 3u) << pointer;
  }
  for (const char* pointer :
       {"/body/DocFragment", "/body/DocFragment.0", "/body/DocFragment/body", "/body/DocFragment/body.0"}) {
    ASSERT_TRUE(ChapterXPathResolver::resolve(epub, 0, pointer, resolved)) << pointer;
    EXPECT_EQ(resolved.visibleOffset, 0u) << pointer;
  }
}

// ---- Clippings: what KOReader will read between the two ends. The differential test holds these same
// shapes to crengine (fixture hostile_cases, clip_shapes and the cases around it). ------------------
namespace {
ChapterXPathResolver::TextRange locate(const std::string& body, const char* clip, const uint32_t anchor = 0,
                                       const uint16_t word = 0) {
  ChapterXPathResolver::TextRange range;
  EXPECT_TRUE(ChapterXPathResolver::locateText(epubWith("<html><body>" + body + "</body></html>"), 0, clip, anchor,
                                               word, range))
      << clip;
  return range;
}
}  // namespace

TEST(KOReaderClipRange, EndsAreTheFirstCharacterAndJustPastTheLast) {
  const auto range = locate("<p>one two three</p>", "two");
  EXPECT_EQ(range.pos0, "/body/DocFragment/body/p/text().4");
  EXPECT_EQ(range.pos1, "/body/DocFragment/body/p/text().7");
  EXPECT_EQ(range.text, "two");
}

TEST(KOReaderClipRange, EndsAtTheEndOfATextNode) {
  const auto range = locate("<p>one <i>two</i> three</p>", "one two");
  EXPECT_EQ(range.pos0, "/body/DocFragment/body/p/text()[1].0");
  EXPECT_EQ(range.pos1, "/body/DocFragment/body/p/i/text().3");
  EXPECT_EQ(range.text, "one two");
}

TEST(KOReaderClipRange, UsesTheDocumentsSpacingNotTheClippings) {
  // The reader's own line and paragraph breaks are not in the source.
  const auto range = locate("<p>word,\n   next</p><p>other</p>", "word, next\nother");
  EXPECT_EQ(range.text, "word, next\nother");
  // Text the reader sets apart and crengine reads together, across an inline boundary.
  EXPECT_EQ(locate("<p>wo<b>rd</b> x</p>", "wo rd x").text, "word x");
}

TEST(KOReaderClipRange, ABlockAndABrSeparateAndInlineElementsDoNot) {
  EXPECT_EQ(locate("<p>a</p><p>b</p>", "a\nb").text, "a\nb");
  EXPECT_EQ(locate("<p>a<br/>b</p>", "a\nb").text, "a\nb");
  EXPECT_EQ(locate("<p>a<i>b</i>c</p>", "abc").text, "abc");
  // Text after a block in the same parent is boxed by crengine, which starts a new block too.
  EXPECT_EQ(locate("<div><p>a</p>b</div>", "a\nb").text, "a\nb");
}

TEST(KOReaderClipRange, SoftHyphensAreNotReadAndNbspIsASpace) {
  const auto hyphen = locate("<p>soft\xC2\xADhyphen word</p>", "softhyphen");
  EXPECT_EQ(hyphen.text, "softhyphen");
  EXPECT_EQ(hyphen.pos1, "/body/DocFragment/body/p/text().11");
  const auto nbsp = locate(
      "<p>a\xC2\xA0"
      "b c</p>",
      "a b");
  EXPECT_EQ(nbsp.pos1, "/body/DocFragment/body/p/text().3");
}

TEST(KOReaderClipRange, TheReadersComposedLetterIsTheSourcesBaseAndMark) {
  const auto range = locate("<p>e\xCC\x81 x</p>", "\xC3\xA9");
  EXPECT_EQ(range.pos1, "/body/DocFragment/body/p/text().2");
  EXPECT_EQ(range.text, "e\xCC\x81");
}

TEST(KOReaderClipRange, ListMarkersAreTheReadersAlone) {
  EXPECT_EQ(locate("<ul><li>one</li><li>two</li></ul>", "one\n\xE2\x80\xA2 two").text, "one\ntwo");
  EXPECT_EQ(locate("<ol><li>one</li><li>two</li></ol>", "one\n2. two").text, "one\ntwo");
  EXPECT_EQ(locate("<ul><li>one</li></ul>", "\xE2\x80\xA2 one").text, "one");
}

TEST(KOReaderClipRange, AnImagesStandInIsNotInTheSource) {
  const auto range = locate("<p>a<img src=\"x\" alt=\"pic\"/>b</p>", "a[Image: pic]b");
  EXPECT_EQ(range.pos0, "/body/DocFragment/body/p/text()[1].0");
  EXPECT_EQ(range.pos1, "/body/DocFragment/body/p/text()[2].1");
  EXPECT_EQ(range.text, "ab");
}

TEST(KOReaderClipRange, TextTheReaderLeavesOutStaysInTheRange) {
  // A page-break marker is not on the reader's page; crengine reads it.
  EXPECT_EQ(locate("<p>a <span epub:type=\"pagebreak\">3</span>b</p>", "a b").text, "a 3b");
  // Ruby annotations are the reverse: neither of them shows it here.
  EXPECT_EQ(locate("<p>a<ruby>k<rt>note</rt></ruby>b</p>", "akb").text, "akb");
  // And text no stylesheet would hide, kept hidden by an inline style, is read by neither.
  EXPECT_EQ(locate("<p>a<span style=\"display:none\">x</span>b</p>", "ab").text, "ab");
}

TEST(KOReaderClipRange, ANumericReferenceInTheC1RangeIsWhatCrengineMakesOfIt) {
  const auto range = locate("<p>a&#150;b</p>", "ab");
  EXPECT_EQ(range.text,
            "a\xE2\x80\x93"
            "b");
}

TEST(KOReaderClipRange, RepeatedTextGoesByTheWordOnThePage) {
  const std::string body = "<p>the cat and the dog and the bird</p>";
  EXPECT_EQ(locate(body, "the", 0, 0).pos0, "/body/DocFragment/body/p/text().0");
  EXPECT_EQ(locate(body, "the", 0, 3).pos0, "/body/DocFragment/body/p/text().12");
  EXPECT_EQ(locate(body, "the", 0, 6).pos0, "/body/DocFragment/body/p/text().24");
  // From the page's own start, the words before it do not count.
  EXPECT_EQ(locate(body, "the", 12, 3).pos0, "/body/DocFragment/body/p/text().24");
}

TEST(KOReaderClipRange, WithoutAnAnchorOnlyUniqueTextIsAccepted) {
  const auto epub = epubWith("<html><body><p>one two one</p></body></html>");
  ChapterXPathResolver::TextRange range;
  EXPECT_TRUE(ChapterXPathResolver::locateText(epub, 0, "two", UINT32_MAX, 0, range));
  EXPECT_FALSE(ChapterXPathResolver::locateText(epub, 0, "one", UINT32_MAX, 0, range));
}

TEST(KOReaderClipRange, OtherTextIsRefusedRatherThanPlacedNearby) {
  const auto epub = epubWith("<html><body><p>one two three</p></body></html>");
  ChapterXPathResolver::TextRange range;
  EXPECT_FALSE(ChapterXPathResolver::locateText(epub, 0, "two four", 0, 0, range));
  EXPECT_FALSE(ChapterXPathResolver::locateText(epub, 0, "", 0, 0, range));
  EXPECT_FALSE(ChapterXPathResolver::locateText(epub, 0, "\xE2\x80\xA2", 0, 0, range));
  EXPECT_FALSE(ChapterXPathResolver::locateText(epub, 0, "[Image: x]", 0, 0, range));
  // Before the anchor is not where the page says it is.
  EXPECT_FALSE(ChapterXPathResolver::locateText(epub, 0, "one", 4, 0, range));
}
