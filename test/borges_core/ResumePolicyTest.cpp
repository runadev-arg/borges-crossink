#include "ResumePolicy.h"
#include "TestSupport.h"

using namespace borges;
using namespace borges_test;
using resume::Verdict;

namespace {

std::unique_ptr<RemoteEvent> makeEvent() {
  auto e = std::make_unique<RemoteEvent>();
  copyText(e->eventId, sizeof(e->eventId), "evt-1");
  e->hasEventId = true;
  e->hasSeq = true;
  e->serverSeq = 10;
  copyText(e->bookKind, sizeof(e->bookKind), "koreader_partial_md5");
  copyText(e->bookValue, sizeof(e->bookValue), kMd5A);
  copyText(e->originId, sizeof(e->originId), kOtherId);
  e->readingHead = true;
  e->hasPct = true;
  e->pctMicro = 50000000;
  return e;
}

using Mutate = void (*)(RemoteEvent&, BookLedger&);

struct DecisionCase {
  const char* name;
  bool nullEvent;
  Mutate mutate;
  Verdict expected;
};

}  // namespace

// Design section 5, row by row.
TEST(ResumePolicy, DecisionTable) {
  const DecisionCase cases[] = {
      {"no focus event", true, nullptr, Verdict::NoEvent},
      {"another device, never answered: ask", false, nullptr, Verdict::Ask},
      {"same spot as here: still ask", false,
       [](RemoteEvent& e, BookLedger& l) {
         l.lastFinalized.pctMicro = e.pctMicro;
         l.hasFinalized = true;
       },
       Verdict::Ask},
      {"from this device", false, [](RemoteEvent& e, BookLedger&) { copyText(e.originId, sizeof(e.originId), kSelfId); }, Verdict::Own},
      {"answered, not read since", false,
       [](RemoteEvent& e, BookLedger& l) {
         l.activityGen = 3;
         l.remember(resume::fingerprint(e), 3);
       },
       Verdict::AlreadyChosen},
      {"answered, read since: ask again", false,
       [](RemoteEvent& e, BookLedger& l) {
         l.remember(resume::fingerprint(e), 3);
         l.activityGen = 4;
       },
       Verdict::Ask},
      {"new event at the same spot asks again", false,
       [](RemoteEvent& e, BookLedger& l) {
         l.activityGen = 3;
         l.remember(resume::fingerprint(e), 3);
         copyText(e.eventId, sizeof(e.eventId), "evt-2");
       },
       Verdict::Ask},
      {"wrong book", false, [](RemoteEvent& e, BookLedger&) { copyText(e.bookValue, sizeof(e.bookValue), kMd5B); }, Verdict::Invalid},
      {"wrong identifier kind", false, [](RemoteEvent& e, BookLedger&) { copyText(e.bookKind, sizeof(e.bookKind), "sha256"); }, Verdict::Invalid},
      {"missing event_id", false,
       [](RemoteEvent& e, BookLedger&) {
         e.hasEventId = false;
         e.eventId[0] = '\0';
       },
       Verdict::Invalid},
      {"missing reading_head", false, [](RemoteEvent& e, BookLedger&) { e.readingHead = false; }, Verdict::Invalid},
      {"missing origin", false, [](RemoteEvent& e, BookLedger&) { e.originId[0] = '\0'; }, Verdict::Invalid},
      {"nowhere to go", false,
       [](RemoteEvent& e, BookLedger&) {
         e.hasPct = false;
         e.xpointerUsable = false;
       },
       Verdict::Invalid},
      {"own wins over a remembered answer", false,
       [](RemoteEvent& e, BookLedger& l) {
         copyText(e.originId, sizeof(e.originId), kSelfId);
         l.remember(resume::fingerprint(e), 0);
       },
       Verdict::Own},
  };
  const BookId book = Device::book();
  for (const DecisionCase& c : cases) {
    auto event = makeEvent();
    auto ledger = std::make_unique<BookLedger>();
    if (c.mutate) c.mutate(*event, *ledger);
    EXPECT_EQ(resume::evaluate(c.nullEvent ? nullptr : event.get(), book, kSelfId, *ledger), c.expected) << c.name;
  }
}

TEST(ResumePolicy, FortyChoicesAreKeptAndTheOldestGoes) {
  auto ledger = std::make_unique<BookLedger>();
  const BookId book = Device::book();
  auto oldest = makeEvent();
  ledger->remember(resume::fingerprint(*oldest), 0);
  for (int i = 2; i <= 40; ++i) {
    auto e = makeEvent();
    const std::string id = "evt-" + std::to_string(i);
    copyText(e->eventId, sizeof(e->eventId), id.c_str());
    ledger->remember(resume::fingerprint(*e), 0);
  }
  EXPECT_EQ(resume::evaluate(oldest.get(), book, kSelfId, *ledger), Verdict::AlreadyChosen);
  auto e41 = makeEvent();
  copyText(e41->eventId, sizeof(e41->eventId), "evt-41");
  ledger->remember(resume::fingerprint(*e41), 0);
  EXPECT_EQ(resume::evaluate(oldest.get(), book, kSelfId, *ledger), Verdict::Ask);
  EXPECT_EQ(resume::evaluate(e41.get(), book, kSelfId, *ledger), Verdict::AlreadyChosen);
}

TEST(ResumePolicy, FingerprintPrefersEventThenSequenceThenPosition) {
  auto a = makeEvent();
  auto b = makeEvent();
  b->serverSeq = 99;  // Different sequence, same event id.
  EXPECT_EQ(resume::fingerprint(*a), resume::fingerprint(*b));

  a->hasEventId = false;
  b->hasEventId = false;
  EXPECT_NE(resume::fingerprint(*a), resume::fingerprint(*b));
  b->serverSeq = a->serverSeq;
  EXPECT_EQ(resume::fingerprint(*a), resume::fingerprint(*b));

  a->hasSeq = false;
  b->hasSeq = false;
  EXPECT_EQ(resume::fingerprint(*a), resume::fingerprint(*b));
  b->pctMicro = 1;
  EXPECT_NE(resume::fingerprint(*a), resume::fingerprint(*b));
}

namespace {
struct WarningCase {
  const char* name;
  bool hasRemote;
  double remote;
  bool hasLocal;
  double local;
  bool xpointer;
  std::vector<std::string> copy;
};

uint32_t micro(double percent) { return static_cast<uint32_t>(percent * 1000000.0 + 0.5); }

const char kBack[] = "Ir ah\xC3\xAD te lleva hacia atr\xC3\xA1s en el libro.";
const char kClose[] = "La diferencia entre las dos posiciones es chica.";
const char kApprox[] = "La ubicaci\xC3\xB3n es aproximada: puede caer unos renglones antes o despu\xC3\xA9s.";
}  // namespace

// Exact wording comes from design section 5.
TEST(ResumePolicy, SpanishWarningSelection) {
  const WarningCase cases[] = {
      {"far ahead, exact", true, 50, true, 10, true, {}},
      {"far behind", true, 10, true, 50, true, {kBack}},
      {"just past the backwards line", true, 49.499999, true, 50, true, {kBack}},
      {"exactly at the backwards line is only close", true, 49.5, true, 50, true, {kClose}},
      {"small step back", true, 49.9, true, 50, true, {kClose}},
      {"same spot", true, 50, true, 50, true, {kClose}},
      {"2% ahead is close", true, 52, true, 50, true, {kClose}},
      {"just past 2% ahead", true, 52.000001, true, 50, true, {}},
      {"2% behind warns backwards, not close", true, 48, true, 50, true, {kBack}},
      {"approximate alone", true, 90, true, 10, false, {kApprox}},
      {"backwards and approximate, in order", true, 10, true, 90, false, {kBack, kApprox}},
      {"close and approximate, in order", true, 50, true, 50, false, {kClose, kApprox}},
      {"no local percentage: nothing to compare", true, 50, false, 0, true, {}},
      {"no remote percentage", false, 0, true, 50, true, {}},
      {"no local and no xpointer", true, 50, false, 0, false, {kApprox}},
  };
  for (const WarningCase& c : cases) {
    const uint8_t mask = resume::warningsFor(c.hasRemote, micro(c.remote), c.hasLocal, micro(c.local), c.xpointer);
    const char* out[3] = {nullptr, nullptr, nullptr};
    const size_t count = resume::warningCopy(mask, out);
    ASSERT_EQ(count, c.copy.size()) << c.name;
    for (size_t i = 0; i < count; ++i) EXPECT_EQ(std::string(out[i]), c.copy[i]) << c.name;
  }
}

TEST(ResumePolicy, PromptCopyIsTheDesignsWording) {
  EXPECT_STREQ(resume::kCopyTitle, "\xC2\xBF" "D\xC3\xB3" "nde quer\xC3\xA9" "s seguir leyendo?");
  EXPECT_STREQ(resume::kCopyGoThere, "Ir a esa posici\xC3\xB3n");
  EXPECT_STREQ(resume::kCopyStayHere, "Seguir aqu\xC3\xAD");
  EXPECT_STREQ(resume::kCopyHereLabel, "En este lector");
}

TEST(ResumePolicy, DatesOnlyFromTrustworthyClocks) {
  EXPECT_TRUE(resume::showsDate(TimePrecision::Exact));
  EXPECT_TRUE(resume::showsDate(TimePrecision::Anchored));
  EXPECT_FALSE(resume::showsDate(TimePrecision::Approximate));
  EXPECT_FALSE(resume::showsDate(TimePrecision::Unknown));
}

TEST(ClockPolicy, ServerTimeOnlyFixesWhatIsWorse) {
  EXPECT_TRUE(clockpolicy::shouldApplyServerTime(WallTime{0, TimePrecision::Unknown}));
  EXPECT_TRUE(clockpolicy::shouldApplyServerTime(WallTime{kNow, TimePrecision::Approximate}));
  EXPECT_TRUE(clockpolicy::shouldApplyServerTime(WallTime{kNow, TimePrecision::Anchored}));
  EXPECT_FALSE(clockpolicy::shouldApplyServerTime(WallTime{kNow, TimePrecision::Exact}));
}

TEST(ClockPolicy, LabelsAreHonest) {
  EXPECT_EQ(clockpolicy::labelAfterBoot(true, 0, 0), TimePrecision::Exact);
  EXPECT_EQ(clockpolicy::labelAfterBoot(false, kNow + 100, kNow), TimePrecision::Anchored);
  EXPECT_EQ(clockpolicy::labelAfterBoot(false, kNow, kNow), TimePrecision::Anchored);
  // A clock behind its own anchor, or no anchor at all, is not a clock.
  EXPECT_EQ(clockpolicy::labelAfterBoot(false, kNow - 1, kNow), TimePrecision::Unknown);
  EXPECT_EQ(clockpolicy::labelAfterBoot(false, kNow, 0), TimePrecision::Unknown);
}
