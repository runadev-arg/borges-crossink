#include "SyncEngine.h"

#include <cstring>
#include <new>

#include "Backoff.h"
#include "JsonStream.h"
#include "net/HttpDate.h"

namespace borges {

namespace {

constexpr const char* kPullPath = "/api/sync/v2/pull";
constexpr const char* kExchangePath = "/api/sync/v2/exchange";
constexpr const char* kCommitPath = "/api/sync/v2/cursor/commit";
constexpr const char* kCapabilitiesPath = "/api/sync/v2/capabilities";
constexpr const char* kHealthPath = "/api/devices/self/health";
constexpr const char* kRotatePath = "/api/devices/self/credentials/rotate";
constexpr int64_t kRotateMarginSec = 14 * 24 * 3600;
constexpr int64_t kShortestCredentialTtlSec = 60;  // normalizeRotationTtlSeconds refuses less.
constexpr uint32_t kDisconnectBudgetMs = 15000;
constexpr uint8_t kLinkFailuresBeforeSkipping = 2;
constexpr uint8_t kLinkSkips = 5;
constexpr size_t kPullBodyBytes = 448;  // protocol, cursor, focus book, limit and the client block.

bool isSequenceConflict(const ApiResult& r) { return r.status == 409 && strcmp(r.code, "client_sequence_conflict") == 0; }

bool isCursorRecoverable(const ApiResult& r) {
  return r.status == 409 && (strcmp(r.code, "cursor_not_delivered") == 0 || strcmp(r.code, "cursor_ahead_of_server") == 0);
}

// Answers that say "this event, as it stands, will never be accepted". With a batch
// the culprit is unknown, so the batch is split; alone it goes to quarantine instead
// of blocking the queue forever. Its bytes stay in outbox.bad.
//
// A 422 only counts when the server names an event as the problem. One about the
// request itself (client.firmware_version, say) would fail for every event, and must
// not empty the queue one by one. The per-session cap is the last line of defence.
constexpr uint8_t kMaxQuarantinesPerSession = 4;

bool validationNamesAnEvent(IFs& fs) {
  File f(fs, kSpoolPath, IFs::Mode::Read);
  if (!f) return false;
  FileByteSource source(f);
  JsonScanner scan(source);
  char key[16];
  bool named = false;
  if (!scan.enterObject()) return false;
  while (scan.nextMember(key, sizeof(key))) {
    if (strcmp(key, "error") != 0 || !scan.enterObject()) {
      if (strcmp(key, "error") != 0) scan.skipValue();
      continue;
    }
    while (scan.nextMember(key, sizeof(key))) {
      if (strcmp(key, "details") != 0 || !scan.enterArray()) {
        if (strcmp(key, "details") != 0) scan.skipValue();
        continue;
      }
      while (scan.nextElement()) {
        if (!scan.enterObject()) {
          scan.skipValue();
          continue;
        }
        while (scan.nextMember(key, sizeof(key))) {
          if (strcmp(key, "path") == 0) {
            char path[16];
            if (scan.readString(path, sizeof(path)) && strncmp(path, "events[", 7) == 0) named = true;
          } else {
            scan.skipValue();
          }
        }
      }
    }
  }
  return named && !scan.failed();
}

bool isPoisonAnswer(const ApiResult& r, IFs& fs) {
  if (r.status == 413) return true;
  if (r.status == 409) return strcmp(r.code, "idempotency_conflict") == 0;
  return r.status == 422 && validationNamesAnEvent(fs);
}

}  // namespace

struct SyncEngine::Scratch {
  RemoteEvent event;
  BookLedger ledger;
  Batch batch;
};

struct SyncEngine::PageInfo {
  int64_t protocolVersion = 0;
  bool hasCursor = false;
  uint64_t cursor = 0;
  bool hasMore = false;
  bool hasEvents = false;
  uint32_t eventsOffset = 0;
  bool hasAcks = false;
  uint32_t acksOffset = 0;
  bool hasFocusEvent = false;  // Non-null focus_event.
  uint32_t focusOffset = 0;
  bool headChecked = false;
  bool hasServerTime = false;
  int64_t serverTime = 0;
};

SyncEngine::SyncEngine(Store& store, Outbox& outbox, IFs& fs, IHttp& http, IClock& clock, IRandom& random,
                       ICancel& cancel, const EngineConfig& config)
    : store_(store), outbox_(outbox), fs_(fs), clock_(clock), random_(random), api_(fs, http, clock, cancel), config_(config) {
  if (config_.maxEventsPerBatch > kMaxBatchEvents) config_.maxEventsPerBatch = kMaxBatchEvents;
  if (config_.maxEventsPerBatch == 0) config_.maxEventsPerBatch = 1;
}

SyncEngine::~SyncEngine() { end(); }

bool SyncEngine::begin() {
  if (!scratch_) scratch_ = new (std::nothrow) Scratch;
  if (!scratch_) return false;
  ledgerLoaded_ = false;
  ledgerDirty_ = false;
  if (!outbox_.isOpen() && !outbox_.open()) return false;
  return true;
}

void SyncEngine::end() {
  delete scratch_;
  scratch_ = nullptr;
}

Gate SyncEngine::gate(SessionKind kind) const {
  const Credentials& creds = store_.creds();
  if (!creds.paired()) return Gate::NotPaired;
  if (kind == SessionKind::Manual) return Gate::Go;
  const SyncState& s = store_.state();
  if (s.needsRelink) return Gate::NeedsRelink;
  if (backoff::blocked(clock_.wallNow(), s.nextAttemptEpoch)) return Gate::BackedOff;
  if (kind == SessionKind::Sleep && outbox_.isOpen() && !hasSleepWork()) return Gate::NothingToDo;
  if (s.linkSkipsLeft > 0) return Gate::BackedOff;
  return Gate::Go;
}

// Events that would actually go up, or a held book with news to ask about. Events held for
// an answer only the reader can give are not work: they would power the radio at every
// sleep, for as long as the book stays unopened.
bool SyncEngine::hasSleepWork() const {
  const HoldSet& holds = store_.state().holds;
  if (outbox_.sendableDepth(&holds) > 0) return true;
  for (uint8_t i = 0; i < holds.size(); ++i) {
    if (!holds.checked(i) && outbox_.hasEventFor(bookTagOf(holds.at(i)), EventKind::Progress)) return true;
  }
  return false;
}

Gate SyncEngine::decideAutomatic(SessionKind kind) {
  const Gate decided = gate(kind);
  SyncState& s = store_.state();
  if (decided == Gate::BackedOff && s.linkSkipsLeft > 0 && !backoff::blocked(clock_.wallNow(), s.nextAttemptEpoch)) {
    --s.linkSkipsLeft;
    store_.saveState();
  }
  return decided;
}

void SyncEngine::noteLinkFailure() {
  SyncState& s = store_.state();
  if (s.linkFailStreak < 250) ++s.linkFailStreak;
  if (s.linkFailStreak >= kLinkFailuresBeforeSkipping) s.linkSkipsLeft = kLinkSkips;
  store_.saveState();
}

bool SyncEngine::onBookOpened(const BookId& book) {
  if (!book.valid() || !store_.creds().paired()) return true;
  SyncState& s = store_.state();
  if (!s.holds.add(book.md5)) return true;
  return store_.saveState();
}

// ---------------------------------------------------------------------------------
// Failure bookkeeping
// ---------------------------------------------------------------------------------

void SyncEngine::setDiag(SyncStage stage, const char* code, const char* detail, const ApiResult& result) {
  Diagnostics& d = store_.state().diag;
  d.stage = stage;
  d.httpStatus = static_cast<int16_t>(result.status);
  d.transportStage = result.stage;
  d.transportCode = result.transportCode;
  d.atEpoch = clock_.wallNow().epochSec;
  copyText(d.code, sizeof(d.code), code);
  copyText(d.detail, sizeof(d.detail), detail);
  copyText(d.requestId, sizeof(d.requestId), result.requestId);
}

void SyncEngine::noteInterruption(SessionReport& report) {
  report.outcome = api_.cancelled() ? SessionOutcome::Cancelled : SessionOutcome::BudgetExhausted;
}

void SyncEngine::recordFailure(SyncStage stage, const ApiResult& r, SessionReport& report) {
  SyncState& s = store_.state();
  switch (r.kind) {
    case ApiKind::Cancelled:
      report.outcome = SessionOutcome::Cancelled;
      return;
    case ApiKind::Budget:
      report.outcome = SessionOutcome::BudgetExhausted;
      return;
    case ApiKind::LocalIo:
      recordLocalError(stage, "spool_io", report);
      return;
    case ApiKind::Transport:
      report.outcome = SessionOutcome::Transport;
      setDiag(stage, "transport", "No response from the server", r);
      break;
    default:
      if (r.status == 401 || r.status == 403) {
        // Keep the queue, stop automatic sessions, and say so. Not a backoff case: it
        // does not heal by waiting.
        s.needsRelink = true;
        report.outcome = SessionOutcome::NeedsRelink;
        setDiag(stage, r.code[0] ? r.code : "unauthorized", r.message, r);
        store_.saveState();
        return;
      }
      report.outcome = r.status == 429 ? SessionOutcome::RateLimited : SessionOutcome::ServerError;
      setDiag(stage, r.code[0] ? r.code : (r.status == 429 ? "rate_limited" : "http_error"), r.message, r);
      break;
  }
  if (s.failures < 250) ++s.failures;
  const uint32_t delay = backoff::delaySec(static_cast<uint8_t>(s.failures - 1), random_);
  s.nextAttemptEpoch = backoff::nextAttempt(clock_.wallNow(), delay, r.hasRetryAfter, r.retryAfterSec);
  store_.saveState();
}

void SyncEngine::recordProtocolError(SyncStage stage, const char* code, const ApiResult& r, SessionReport& report) {
  report.outcome = SessionOutcome::ProtocolError;
  setDiag(stage, code, "Unexpected server response", r);
  SyncState& s = store_.state();
  if (s.failures < 250) ++s.failures;
  s.nextAttemptEpoch = backoff::nextAttempt(clock_.wallNow(), backoff::delaySec(static_cast<uint8_t>(s.failures - 1), random_),
                                            false, 0);
  store_.saveState();
}

void SyncEngine::recordLocalError(SyncStage stage, const char* code, SessionReport& report) {
  report.outcome = SessionOutcome::LocalError;
  setDiag(stage, code, "Could not write to the SD card", ApiResult{});
  store_.saveState();
}

// ---------------------------------------------------------------------------------
// Session frame
// ---------------------------------------------------------------------------------

bool SyncEngine::startSession(SessionKind kind, SessionReport& report) {
  currentKind_ = kind;
  ledgerLoaded_ = false;
  ledgerDirty_ = false;
  if (!scratch_ && !begin()) {
    report.outcome = SessionOutcome::LocalError;
    return false;
  }
  if (!outbox_.isOpen() && !outbox_.open()) {
    report.outcome = SessionOutcome::LocalError;
    return false;
  }
  if (!store_.creds().paired()) {
    report.outcome = SessionOutcome::NotPaired;
    return false;
  }
  const uint32_t budget = kind == SessionKind::Open    ? config_.openBudgetMs
                          : kind == SessionKind::Sleep ? config_.sleepBudgetMs
                                                       : config_.manualBudgetMs;
  // Whoever calls this has a link.
  store_.state().linkFailStreak = 0;
  store_.state().linkSkipsLeft = 0;
  api_.configure(store_.creds().baseUrl, store_.creds().token);
  api_.beginSession(budget);
  return true;
}

SessionReport SyncEngine::finish(SessionReport report, SessionKind kind) {
  flushLedger();
  if (outbox_.isOpen()) outbox_.compactIfNeeded();
  report.depth = outbox_.depth();
  report.held = report.depth - outbox_.sendableDepth(&store_.state().holds);

  SyncState& s = store_.state();
  if (report.outcome == SessionOutcome::Ok) {
    if (kind != SessionKind::Open) maybeRotate(report);
    s.failures = 0;
    s.nextAttemptEpoch = 0;
    s.needsRelink = false;
    if (!report.focusInvalid) s.diag.clear();
    const WallTime now = clock_.wallNow();
    if (now.precision != TimePrecision::Unknown) s.lastSuccessEpoch = now.epochSec;
  }
  if (kind != SessionKind::Open) reportHealth(report);
  store_.saveState();
  api_.endSession();
  return report;
}

// Pull before push (rule 9): if the pull fails, drain does not run, or stale progress
// would become the latest write.
void SyncEngine::pullAndDrain(SessionReport& report) {
  if (!pullPages(report)) {
    report.pullFailed = true;
    report.drainBlockedByPull = outbox_.sendableDepth(&store_.state().holds) > 0;
    return;
  }
  if (!drain(report)) return;
  commitCursor(report);
}

SessionReport SyncEngine::runOpenSession(const BookRef& book, const Position& local, ResumeDecision& out) {
  SessionReport report;
  out = ResumeDecision{};
  pendingChoice_ = false;
  if (!startSession(SessionKind::Open, report)) return report;
  if (!onBookOpened(book.id)) {
    recordLocalError(SyncStage::Focus, "state_write", report);
    return finish(report, SessionKind::Open);
  }
  if (!focus(book.id, &local, &out, report)) return finish(report, SessionKind::Open);
  if (out.ask) {
    // The connection stays up for applyChoice(); the prompt must not wait on history.
    report.outcome = SessionOutcome::AwaitingChoice;
    report.depth = outbox_.depth();
    report.held = report.depth - outbox_.sendableDepth(&store_.state().holds);
    store_.saveState();
    return report;
  }
  pullAndDrain(report);
  return finish(report, SessionKind::Open);
}

SessionReport SyncEngine::runSleepSession() {
  SessionReport report;
  pendingChoice_ = false;
  if (!startSession(SessionKind::Sleep, report)) return report;
  if (checkHeldBooks(nullptr, report)) pullAndDrain(report);
  return finish(report, SessionKind::Sleep);
}

SessionReport SyncEngine::runManualSession(const BookRef* book, const Position* local, ResumeDecision* out) {
  SessionReport report;
  pendingChoice_ = false;
  if (out) *out = ResumeDecision{};
  if (!startSession(SessionKind::Manual, report)) return report;
  if (book) {
    if (!onBookOpened(book->id)) {
      recordLocalError(SyncStage::Focus, "state_write", report);
      return finish(report, SessionKind::Manual);
    }
    if (!focus(book->id, local, out, report)) return finish(report, SessionKind::Manual);
    if (out && out->ask) {
      report.outcome = SessionOutcome::AwaitingChoice;
      report.depth = outbox_.depth();
      report.held = report.depth - outbox_.sendableDepth(&store_.state().holds);
      store_.saveState();
      return report;
    }
  }
  if (checkHeldBooks(book ? &book->id : nullptr, report)) pullAndDrain(report);
  return finish(report, SessionKind::Manual);
}

SessionReport SyncEngine::abandonChoice() {
  SessionReport report;
  pendingChoice_ = false;
  report.outcome = SessionOutcome::Cancelled;
  report.focusChecked = true;
  report.depth = outbox_.depth();
  report.held = report.depth - outbox_.sendableDepth(&store_.state().holds);
  store_.saveState();
  api_.endSession();
  return report;
}

SessionReport SyncEngine::applyChoice(const ResumeDecision& decision, ResumeChoice choice, const Position& before,
                                      const Position& after, bool online) {
  SessionReport report;
  if (!pendingChoice_ || decision.fingerprint != pendingFingerprint_ ||
      memcmp(decision.book.md5, pendingBook_.md5, 16) != 0) {
    report.outcome = SessionOutcome::StaleChoice;
    return report;
  }
  const SessionKind kind = currentKind_;
  BookLedger& ledger = scratch_->ledger;
  ledgerLoaded_ = false;
  if (!store_.loadBook(decision.book, ledger)) {
    recordLocalError(SyncStage::Focus, "ledger_read", report);
    return report;
  }

  EventTime now;
  now.wall = clock_.wallNow();
  now.hasMonotonic = true;
  now.monotonicMs = clock_.monotonicMs();
  const Position& position = choice == ResumeChoice::GoThere ? after : before;

  // Order matters for a crash in between: the event first, so losing the steps below
  // only means asking again. Never the other way around, where a remembered answer
  // would have no event behind it.
  if (!enqueueProgress(decision.book, now, position, choice == ResumeChoice::GoThere)) {
    recordLocalError(SyncStage::Focus, "outbox_write", report);
    return report;
  }
  if (choice == ResumeChoice::GoThere) {
    ledger.undo = before;
    ledger.hasUndo = true;
    if (ledger.suggestionId[0]) ledger.suggestionAction = SuggestionAction::Accept;
  } else {
    // This device becomes the head: a read that happened now.
    if (ledger.suggestionId[0]) ledger.suggestionAction = SuggestionAction::Dismiss;
  }
  ledger.remember(decision.fingerprint);
  if (position.hasPct) {
    ledger.lastFinalized = position;
    ledger.hasFinalized = true;
    ledger.lastTurnEpoch = now.wall.epochSec;
  }
  if (!store_.saveBook(ledger)) {
    recordLocalError(SyncStage::Focus, "ledger_write", report);
    return report;
  }
  store_.state().holds.remove(decision.book.md5);
  if (!store_.saveState()) {
    recordLocalError(SyncStage::Focus, "state_write", report);
    return report;
  }
  pendingChoice_ = false;
  report.focusChecked = true;

  if (!online) {
    report.depth = outbox_.depth();
    report.held = report.depth - outbox_.sendableDepth(&store_.state().holds);
    return report;
  }

  if (kind == SessionKind::Open) {
    api_.beginSession(config_.openBudgetMs);
  } else {
    api_.beginSession(config_.manualBudgetMs);
  }
  resolveSuggestion(decision.book);
  pullAndDrain(report);
  return finish(report, kind);
}

// ---------------------------------------------------------------------------------
// Focus check
// ---------------------------------------------------------------------------------

// On every request that takes one, as the plugin does: the server overwrites queue_depth
// with what each request says, and a request with no client block says zero.
void SyncEngine::writeClient(JsonWriter& w) const {
  w.key("client");
  w.objectBegin();
  w.kvInt("protocol_version", 2);
  if (config_.firmwareVersion[0]) w.kv("firmware_version", config_.firmwareVersion);
  if (config_.clientVersion[0]) w.kv("client_version", config_.clientVersion);
  w.kvInt("queue_depth", static_cast<int64_t>(outbox_.depth()));
  w.key("capabilities");
  w.objectBegin();
  w.kvBool("atomic_sd_outbox", true);
  w.objectEnd();
  w.objectEnd();
}

bool SyncEngine::focus(const BookId& book, const Position* local, ResumeDecision* out, SessionReport& report) {
  char body[kPullBodyBytes];
  JsonWriter w(body, sizeof(body));
  w.objectBegin();
  w.kvInt("protocol_version", 2);
  w.kvSeq("cursor", store_.state().cursor);
  w.key("focus_book");
  w.objectBegin();
  w.kv("kind", "koreader_partial_md5");
  w.kv("value", book.hex);
  w.objectEnd();
  w.kvBool("focus_only", true);
  writeClient(w);
  w.objectEnd();
  if (!w.ok()) {
    recordLocalError(SyncStage::Focus, "body_overflow", report);
    return false;
  }

  const ApiResult r = api_.call("POST", kPullPath, reinterpret_cast<const uint8_t*>(body), w.size());
  if (r.kind != ApiKind::Ok) {
    recordFailure(SyncStage::Focus, r, report);
    return false;
  }

  File f(fs_, kSpoolPath, IFs::Mode::Read);
  if (!f) {
    recordLocalError(SyncStage::Focus, "spool_read", report);
    return false;
  }
  PageInfo page;
  if (!scanPage(f, page)) {
    recordProtocolError(SyncStage::Focus, "malformed_response", r, report);
    return false;
  }
  noteServerTime(page);

  if (!page.headChecked) {
    // A server that ignored focus_only sent a normal page. Process it, lose nothing,
    // and report that the question has no answer: the book stays held.
    if (page.hasEvents && page.hasCursor && !applyPage(f, page, SyncStage::Focus, r, report)) return false;
    report.focusInvalid = true;
    setDiag(SyncStage::Focus, "reading_head_unavailable", "The server did not answer the check", r);
    return true;
  }
  report.focusChecked = true;

  resume::Verdict verdict = resume::Verdict::NoEvent;
  RemoteEvent& event = scratch_->event;
  BookLedger& ledger = scratch_->ledger;
  ledgerLoaded_ = false;
  if (!store_.loadBook(book, ledger)) {
    recordLocalError(SyncStage::Focus, "ledger_read", report);
    return false;
  }
  if (page.hasFocusEvent) {
    FileByteSource source(f);
    JsonScanner scan(source);
    scan.seekTo(page.focusOffset);
    if (!parseRemoteEvent(scan, event)) {
      recordProtocolError(SyncStage::Focus, "malformed_response", r, report);
      return false;
    }
    verdict = resume::evaluate(&event, book, store_.creds().deviceId, ledger);
    // This device's own head, and nothing local to read from: a card moved between readers,
    // or a store that lost its place. Opening at page 1 and publishing that would overwrite
    // the head, so it is offered like anyone else's.
    if (verdict == resume::Verdict::Own && local && !local->hasPct) verdict = resume::Verdict::Ask;
  } else {
    verdict = resume::evaluate(nullptr, book, store_.creds().deviceId, ledger);
  }

  if (out) {
    out->verdict = verdict;
    out->book = book;
    if (local) out->local = *local;
  }
  switch (verdict) {
    case resume::Verdict::Invalid:
      // A reported error, no prompt. The hold stays, so nothing stale goes up.
      report.focusInvalid = true;
      setDiag(SyncStage::Focus, "reading_head_invalid", "The server's position was not usable", r);
      return true;
    case resume::Verdict::Ask:
      if (out) {
        out->ask = true;
        out->hasRemotePct = event.hasPct;
        out->remotePct = event.pctMicro;
        out->xpointerUsable = event.xpointerUsable;
        copyText(out->remoteXpointer, sizeof(out->remoteXpointer), event.xpointer);
        copyText(out->remoteDeviceName, sizeof(out->remoteDeviceName), event.originName);
        out->showDate = resume::showsDate(event.precision) && event.occurredAt[0];
        if (out->showDate) copyText(out->occurredAt, sizeof(out->occurredAt), event.occurredAt);
        out->warnings = resume::warningsFor(event.hasPct, event.pctMicro, local && local->hasPct,
                                            local ? local->pctMicro : 0, event.xpointerUsable);
        out->fingerprint = resume::fingerprint(event);
        pendingChoice_ = true;
        pendingFingerprint_ = out->fingerprint;
        pendingBook_ = book;
      }
      return true;  // Held: the choice is what releases it.
    default:
      break;
  }
  if (store_.state().holds.remove(book.md5) && !store_.saveState()) {
    recordLocalError(SyncStage::Focus, "state_write", report);
    return false;
  }
  return true;
}

bool SyncEngine::checkHeldBooks(const BookId* except, SessionReport& report) {
  // Oldest first, since release shifts the set. Each is one short round trip; a book
  // that wants a prompt just stays held until it is opened.
  uint8_t checks = 0;
  uint8_t index = 0;
  while (index < store_.state().holds.size() && checks < config_.maxHeldChecks) {
    if (!api_.canContinue()) {
      noteInterruption(report);
      return false;
    }
    BookId book;
    book.setRaw(store_.state().holds.at(index));
    if (except && memcmp(except->md5, book.md5, 16) == 0) {
      ++index;
      continue;
    }
    // Only books with something waiting are worth a request, and at sleep only once for
    // each thing read: the answer will not change until a person opens the book.
    if (!outbox_.hasEventFor(bookTagOf(book.md5), EventKind::Progress) ||
        (currentKind_ == SessionKind::Sleep && store_.state().holds.checked(index))) {
      ++index;
      continue;
    }
    const uint8_t before = store_.state().holds.size();
    if (!focus(book, nullptr, nullptr, report)) return false;
    ++checks;
    if (store_.state().holds.size() == before) {
      store_.state().holds.markChecked(book.md5);
      ++index;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------
// Pull
// ---------------------------------------------------------------------------------

// First pass over a response: validates the whole document's structure and finds
// what it needs by offset. Keys can come in any order and `cursor` follows `events`,
// so nothing is applied until this has seen the end. A truncated or malformed spool
// fails here, with no effect.
bool SyncEngine::scanPage(File& f, PageInfo& page) {
  page = PageInfo{};
  FileByteSource source(f);
  JsonScanner scan(source);
  if (!scan.enterObject()) return false;

  char key[24];
  const auto handle = [&]() {
    if (strcmp(key, "events") == 0) {
      if (scan.peek() == JsonType::Array) {
        page.hasEvents = true;
        page.eventsOffset = scan.offset();
      }
      scan.skipValue();
    } else if (strcmp(key, "cursor") == 0) {
      page.hasCursor = scan.readSequence(page.cursor);
    } else if (strcmp(key, "has_more") == 0) {
      scan.readBool(page.hasMore);
    } else if (strcmp(key, "focus_event") == 0) {
      if (scan.peek() == JsonType::Object) {
        page.hasFocusEvent = true;
        page.focusOffset = scan.offset();
      }
      scan.skipValue();
    } else if (strcmp(key, "reading_head_checked") == 0) {
      scan.readBool(page.headChecked);
    } else if (strcmp(key, "server_time") == 0) {
      char text[40];
      if (scan.readString(text, sizeof(text))) page.hasServerTime = parseRfc3339(text, page.serverTime);
    } else if (strcmp(key, "protocol_version") == 0) {
      scan.readInt(page.protocolVersion);
    } else {
      scan.skipValue();
    }
  };

  while (scan.nextMember(key, sizeof(key))) {
    if (strcmp(key, "acknowledgements") == 0) {
      if (scan.peek() == JsonType::Array) {
        page.hasAcks = true;
        page.acksOffset = scan.offset();
      }
      scan.skipValue();
    } else if (strcmp(key, "pull") == 0) {
      if (scan.enterObject()) {
        while (scan.nextMember(key, sizeof(key))) handle();
      } else {
        scan.skipValue();
      }
    } else {
      handle();
    }
  }
  if (scan.failed() || !scan.atEnd()) return false;
  return page.protocolVersion == 2;
}

void SyncEngine::noteServerTime(const PageInfo& page) {
  if (!page.hasServerTime || !net::epochInBounds(page.serverTime)) return;
  store_.state().anchorEpoch = page.serverTime;
  if (clockpolicy::shouldApplyServerTime(clock_.wallNow())) clock_.setWall(page.serverTime, TimePrecision::Exact);
}

bool SyncEngine::flushLedger() {
  if (!ledgerDirty_) return true;
  ledgerDirty_ = false;
  return store_.saveBook(scratch_->ledger);
}

bool SyncEngine::applyRemote(SessionReport& report) {
  RemoteEvent& e = scratch_->event;
  if (!e.hasSeq) return true;
  const bool relevant = e.isProgress() || e.isAnnotation();
  if (!relevant || strcmp(e.bookKind, "koreader_partial_md5") != 0) return true;
  if (e.originId[0] && strcmp(e.originId, store_.creds().deviceId) == 0) return true;  // Our own, replayed.
  BookId book;
  if (!book.setHex(e.bookValue)) return true;

  // Another device's annotations are not kept: nothing here shows them, and 500 books of
  // them would be 32 MB on the card. Rendering them in the page is out of scope (section 9).
  if (!e.isProgress()) {
    // What is kept is the revision of the ones this device published: deleting one that
    // was edited on the web needs it, or the server sees a stale delete and opens a conflict.
    uint8_t syncId[16];
    if (e.annotationRevision && uuidParse(e.syncId, syncId) && !store_.noteAnnotationRevision(book, syncId, e.annotationRevision)) {
      return false;
    }
    ++report.eventsApplied;
    return true;
  }

  BookLedger& ledger = scratch_->ledger;
  if (!ledgerLoaded_ || memcmp(ledger.id.md5, book.md5, 16) != 0) {
    if (!flushLedger()) return false;
    if (!store_.loadBook(book, ledger)) return false;
    ledgerLoaded_ = true;
  }

  // Never prompts. It only keeps the newest head per book.
  if (e.serverSeq > ledger.head.serverSeq) {
    ledger.head.valid = true;
    ledger.head.serverSeq = e.serverSeq;
    ledger.head.position = Position{};
    ledger.head.position.hasPct = e.hasPct;
    ledger.head.position.pctMicro = e.pctMicro;
    ledger.head.position.page = e.page;
    ledger.head.position.totalPages = e.totalPages;
    if (e.xpointerUsable) copyText(ledger.head.position.xpointer, kMaxXpointer, e.xpointer);
    ledger.head.precision = e.precision;
    copyText(ledger.head.occurredAt, sizeof(ledger.head.occurredAt), e.occurredAt);
    copyText(ledger.head.deviceName, sizeof(ledger.head.deviceName), e.originName);
    ledgerDirty_ = true;
  }
  if (e.suggestionId[0]) {
    copyText(ledger.suggestionId, sizeof(ledger.suggestionId), e.suggestionId);
    ledger.suggestionAction = SuggestionAction::None;
    ledgerDirty_ = true;
  }
  ++report.eventsApplied;
  return true;
}

bool SyncEngine::applyPage(File& f, const PageInfo& page, SyncStage stage, const ApiResult& result, SessionReport& report) {
  FileByteSource source(f);
  JsonScanner scan(source);
  if (page.hasEvents) {
    scan.seekTo(page.eventsOffset);
    if (!scan.enterArray()) {
      recordProtocolError(stage, "malformed_response", result, report);
      return false;
    }
    while (scan.nextElement()) {
      const uint32_t start = scan.offset();
      if (!parseRemoteEvent(scan, scratch_->event)) break;
      const uint32_t size = scan.offset() - start;
      if (size > config_.eventParseBudget) {
        // Never a poison page: skip it, keep going, and leave a trace.
        SyncState& s = store_.state();
        ++s.skippedEvents;
        if (scratch_->event.hasSeq) s.lastSkippedSeq = scratch_->event.serverSeq;
        ++report.eventsSkipped;
        continue;
      }
      if (!applyRemote(report)) {
        recordLocalError(stage, "apply_write", report);
        return false;
      }
      if (!api_.canContinue()) {
        // Nothing persisted a new cursor, so this page simply comes again.
        flushLedger();
        noteInterruption(report);
        return false;
      }
    }
    if (scan.failed()) {
      recordProtocolError(stage, "malformed_response", result, report);
      return false;
    }
  }
  if (!flushLedger()) {
    recordLocalError(stage, "ledger_write", report);
    return false;
  }
  // Applied state is on disk and every event above is idempotent per book, so this
  // single save is the atomic point of the page. Only now can the new cursor be sent.
  SyncState& s = store_.state();
  if (page.hasCursor && page.cursor > s.cursor) s.cursor = page.cursor;
  if (!store_.saveState()) {
    recordLocalError(stage, "state_write", report);
    return false;
  }
  return true;
}

bool SyncEngine::recoverCursor(const ApiResult& result, SessionReport& report) {
  // The server says which cursor it can vouch for. Only ever go back: moving forward
  // on its word would skip events we never applied.
  uint64_t target = 0;
  bool have = false;
  {
    File f(fs_, kSpoolPath, IFs::Mode::Read);
    if (!f) return false;
    FileByteSource source(f);
    JsonScanner scan(source);
    char key[24];
    if (!scan.enterObject()) return false;
    while (scan.nextMember(key, sizeof(key))) {
      if (strcmp(key, "error") != 0 || !scan.enterObject()) {
        if (strcmp(key, "error") != 0) scan.skipValue();
        continue;
      }
      while (scan.nextMember(key, sizeof(key))) {
        if (strcmp(key, "details") != 0 || !scan.enterObject()) {
          if (strcmp(key, "details") != 0) scan.skipValue();
          continue;
        }
        while (scan.nextMember(key, sizeof(key))) {
          if (strcmp(key, "deliveredCursor") == 0 || strcmp(key, "highWatermark") == 0) {
            have = scan.readSequence(target) || have;
          } else {
            scan.skipValue();
          }
        }
      }
    }
  }
  SyncState& s = store_.state();
  if (!have || target >= s.cursor) return false;
  s.cursor = target;
  s.committedCursor = target;
  report.cursorRecovered = true;
  setDiag(SyncStage::Pull, result.code, "Cursor moved back to what the server delivered", result);
  return store_.saveState();
}

bool SyncEngine::pullPages(SessionReport& report) {
  bool recovered = false;
  for (uint8_t pageIndex = 0; pageIndex < config_.maxPullPages;) {
    if (!api_.canContinue()) {
      noteInterruption(report);
      return false;
    }
    const uint64_t requested = store_.state().cursor;
    char body[kPullBodyBytes];
    JsonWriter w(body, sizeof(body));
    w.objectBegin();
    w.kvInt("protocol_version", 2);
    w.kvSeq("cursor", requested);
    w.kvInt("limit", config_.pullLimit);
    writeClient(w);
    w.objectEnd();
    if (!w.ok()) {
      recordLocalError(SyncStage::Pull, "body_overflow", report);
      return false;
    }

    const ApiResult r = api_.call("POST", kPullPath, reinterpret_cast<const uint8_t*>(body), w.size());
    if (r.kind == ApiKind::HttpError && isCursorRecoverable(r) && !recovered && recoverCursor(r, report)) {
      recovered = true;
      continue;
    }
    if (r.kind != ApiKind::Ok) {
      recordFailure(SyncStage::Pull, r, report);
      return false;
    }
    File f(fs_, kSpoolPath, IFs::Mode::Read);
    if (!f) {
      recordLocalError(SyncStage::Pull, "spool_read", report);
      return false;
    }
    PageInfo page;
    if (!scanPage(f, page) || !page.hasEvents || !page.hasCursor || page.cursor < requested) {
      recordProtocolError(SyncStage::Pull, "malformed_response", r, report);
      return false;
    }
    noteServerTime(page);
    const uint16_t appliedBefore = report.eventsApplied;
    const uint16_t skippedBefore = report.eventsSkipped;
    if (!applyPage(f, page, SyncStage::Pull, r, report)) return false;
    // A success with this cursor means the server has acknowledged it.
    store_.state().committedCursor = requested;
    ++report.pagesPulled;
    ++pageIndex;
    if (!page.hasMore) break;
    // A page that moves nothing would spin to the page limit for no reason.
    if (page.cursor == requested && report.eventsApplied == appliedBefore && report.eventsSkipped == skippedBefore) break;
  }
  return true;
}

void SyncEngine::commitCursor(SessionReport& report) {
  SyncState& s = store_.state();
  if (s.cursor == s.committedCursor) return;
  if (!api_.canContinue()) return;  // The next request carries it.
  char body[64];
  JsonWriter w(body, sizeof(body));
  w.objectBegin();
  w.kvInt("protocol_version", 2);
  w.kvSeq("cursor", s.cursor);
  w.objectEnd();
  const ApiResult r = api_.call("POST", kCommitPath, reinterpret_cast<const uint8_t*>(body), w.size());
  if (r.kind == ApiKind::Ok) {
    s.committedCursor = s.cursor;
    return;
  }
  if (r.kind != ApiKind::HttpError) return;  // Cancelled, budget or a drop: everything is applied.
  if (r.status == 401 || r.status == 403) {
    recordFailure(SyncStage::Commit, r, report);
    return;
  }
  if (r.status == 409 && strcmp(r.code, "cursor_regression") == 0) {
    // Our checkpoint is older than the server's (a restored SD card). Keep ours and
    // keep pulling: pulls replay from it. Adopting the server's would skip events.
    setDiag(SyncStage::Commit, r.code, "Catching up from an older checkpoint", r);
    report.cursorRecovered = true;
    return;
  }
  if (isCursorRecoverable(r)) recoverCursor(r, report);
}

// ---------------------------------------------------------------------------------
// Drain
// ---------------------------------------------------------------------------------

bool SyncEngine::ensureCapabilities(SessionReport& report) {
  SyncState& s = store_.state();
  const uint32_t hash = crc32(config_.clientVersion, strlen(config_.clientVersion)) | 1u;
  if (s.capsClientHash == hash) return true;
  const ApiResult r = api_.call("GET", kCapabilitiesPath, nullptr, 0);
  if (r.kind != ApiKind::Ok) {
    recordFailure(SyncStage::Capabilities, r, report);
    return false;
  }
  File f(fs_, kSpoolPath, IFs::Mode::Read);
  if (!f) {
    recordLocalError(SyncStage::Capabilities, "spool_read", report);
    return false;
  }
  FileByteSource source(f);
  JsonScanner scan(source);
  int64_t protocol = 0;
  // An unknown version has no implicit fallback: every event type this client writes
  // must be listed with version 1, or the queue stays closed.
  static constexpr const char* kNeeded[] = {"progress.changed",   "annotation.created", "annotation.deleted",
                                            "bookmark.upserted", "bookmark.deleted",   "session.ended"};
  uint8_t foundMask = 0;
  char key[32];
  if (scan.enterObject()) {
    while (scan.nextMember(key, sizeof(key))) {
      if (strcmp(key, "protocol_version") == 0) {
        scan.readInt(protocol);
      } else if (strcmp(key, "event_versions") == 0 && scan.enterObject()) {
        while (scan.nextMember(key, sizeof(key))) {
          int which = -1;
          for (int i = 0; i < 6; ++i) {
            if (strcmp(key, kNeeded[i]) == 0) which = i;
          }
          if (which >= 0 && scan.enterArray()) {
            while (scan.nextElement()) {
              int64_t version = 0;
              if (scan.readInt(version) && version == 1) foundMask |= static_cast<uint8_t>(1u << which);
            }
          } else {
            scan.skipValue();
          }
        }
      } else {
        scan.skipValue();
      }
    }
  }
  if (scan.failed() || protocol != 2 || foundMask != 0x3F) {
    recordProtocolError(SyncStage::Capabilities, "capabilities_unsupported", r, report);
    return false;
  }
  s.capsClientHash = hash;
  return true;
}

bool SyncEngine::processAcks(File& f, const PageInfo& page, const Batch& batch, const ApiResult& result,
                             SessionReport& report) {
  uint32_t offsets[kMaxBatchEvents];
  size_t count = 0;
  if (page.hasAcks) {
    FileByteSource source(f);
    JsonScanner scan(source);
    scan.seekTo(page.acksOffset);
    if (!scan.enterArray()) {
      recordProtocolError(SyncStage::Drain, "malformed_response", result, report);
      return false;
    }
    while (scan.nextElement()) {
      char id[40] = {0};
      uint64_t seq = 0;
      bool hasSeq = false;
      char status[12] = {0};
      char rejection[40] = {0};
      if (!scan.enterObject()) {
        scan.skipValue();
        continue;
      }
      char key[24];
      while (scan.nextMember(key, sizeof(key))) {
        if (strcmp(key, "client_event_id") == 0) {
          scan.readString(id, sizeof(id));
        } else if (strcmp(key, "client_sequence") == 0) {
          hasSeq = scan.readSequence(seq);
        } else if (strcmp(key, "status") == 0) {
          scan.readString(status, sizeof(status));
        } else if (strcmp(key, "rejection_code") == 0) {
          scan.readString(rejection, sizeof(rejection));
        } else {
          scan.skipValue();
        }
      }
      uint8_t raw[16];
      if (!uuidParse(id, raw)) continue;  // Not one of ours.
      const BatchItem* item = nullptr;
      for (uint8_t i = 0; i < batch.count && !item; ++i) {
        if (memcmp(batch.items[i].id, raw, 16) == 0) item = &batch.items[i];
      }
      if (!item) continue;
      if (!hasSeq || seq != item->seq) {
        recordProtocolError(SyncStage::Drain, "ack_sequence_mismatch", result, report);
        return false;
      }
      const bool accepted = strcmp(status, "accepted") == 0;
      const bool rejected = strcmp(status, "rejected") == 0;
      if (!accepted && !rejected) {
        recordProtocolError(SyncStage::Drain, "ack_status_invalid", result, report);
        return false;
      }
      bool duplicate = false;
      for (size_t i = 0; i < count && !duplicate; ++i) duplicate = offsets[i] == item->offset;
      if (duplicate) continue;
      offsets[count++] = item->offset;
      if (accepted) {
        ++report.eventsAcked;
      } else {
        // Both statuses leave the outbox. A correction would be a new event.
        ++report.eventsRejected;
        SyncState& s = store_.state();
        ++s.rejectedEvents;
        copyText(s.lastRejection, sizeof(s.lastRejection), rejection);
      }
    }
    if (scan.failed()) {
      recordProtocolError(SyncStage::Drain, "malformed_response", result, report);
      return false;
    }
  }
  if (count == 0) {
    recordProtocolError(SyncStage::Drain, "ack_missing", result, report);
    return false;
  }
  if (!outbox_.ack(offsets, count)) {
    recordLocalError(SyncStage::Drain, "outbox_write", report);
    return false;
  }
  return true;
}

// Protocol section 2. The server proves which ids it has never recorded; only those can
// change numbers. Any entry that does not match what was just sent voids the whole
// proof: without explicit confirmation the queue is not touched.
bool SyncEngine::repairSequences(const Batch& batch, SessionReport& report) {
  uint64_t floor = 0;
  bool hasFloor = false;
  bool proofOk = true;
  uint32_t proven[kMaxBatchEvents];
  size_t provenCount = 0;
  {
    File f(fs_, kSpoolPath, IFs::Mode::Read);
    if (!f) return false;
    FileByteSource source(f);
    JsonScanner scan(source);
    char key[28];
    if (!scan.enterObject()) return false;
    while (scan.nextMember(key, sizeof(key))) {
      if (strcmp(key, "error") != 0 || !scan.enterObject()) {
        if (strcmp(key, "error") != 0) scan.skipValue();
        continue;
      }
      while (scan.nextMember(key, sizeof(key))) {
        if (strcmp(key, "details") != 0 || !scan.enterObject()) {
          if (strcmp(key, "details") != 0) scan.skipValue();
          continue;
        }
        while (scan.nextMember(key, sizeof(key))) {
          if (strcmp(key, "next_client_sequence") == 0) {
            hasFloor = scan.readSequence(floor);
          } else if (strcmp(key, "unrecorded_events") == 0 && scan.enterArray()) {
            while (scan.nextElement()) {
              char id[40] = {0};
              uint64_t seq = 0;
              bool hasSeq = false;
              if (scan.enterObject()) {
                char inner[24];
                while (scan.nextMember(inner, sizeof(inner))) {
                  if (strcmp(inner, "client_event_id") == 0) {
                    scan.readString(id, sizeof(id));
                  } else if (strcmp(inner, "client_sequence") == 0) {
                    hasSeq = scan.readSequence(seq);
                  } else {
                    scan.skipValue();
                  }
                }
              } else {
                scan.skipValue();
              }
              uint8_t raw[16];
              const BatchItem* item = nullptr;
              if (uuidParse(id, raw)) {
                for (uint8_t i = 0; i < batch.count && !item; ++i) {
                  if (memcmp(batch.items[i].id, raw, 16) == 0) item = &batch.items[i];
                }
              }
              if (!item || !hasSeq || seq != item->seq || provenCount == kMaxBatchEvents) {
                proofOk = false;
                continue;
              }
              bool seen = false;
              for (size_t i = 0; i < provenCount && !seen; ++i) seen = proven[i] == item->offset;
              if (seen) {
                proofOk = false;
                continue;
              }
              proven[provenCount++] = item->offset;
            }
          } else {
            scan.skipValue();
          }
        }
      }
    }
    if (scan.failed()) return false;
  }
  if (!proofOk || !hasFloor || floor < 1 || provenCount == 0) return false;
  if (!outbox_.renumber(proven, provenCount, floor)) return false;
  report.sequenceRepaired = true;
  return true;
}

bool SyncEngine::drain(SessionReport& report) {
  SyncState& s = store_.state();
  const IHoldFilter* holds = &s.holds;
  if (outbox_.sendableDepth(holds) == 0) return true;
  if (!ensureCapabilities(report)) return false;

  Batch& batch = scratch_->batch;
  size_t maxEvents = config_.maxEventsPerBatch;
  bool repairUsed = false;
  bool cursorRecovered = false;
  uint32_t skip[4];
  size_t skipCount = 0;

  for (;;) {
    if (!api_.canContinue()) {
      noteInterruption(report);
      return false;
    }
    outbox_.selectBatch(maxEvents, config_.maxBodyBytes, holds, skip, skipCount, batch);
    if (batch.count == 0) return true;

    const size_t clientLen = strlen(config_.firmwareVersion) * 2 + strlen(config_.clientVersion) * 2;
    const size_t cap = batch.wireBytes + clientLen + 256;
    char* body = new (std::nothrow) char[cap];
    if (!body) {
      // No contiguous room for this batch. Try it smaller, then let this one event
      // wait so the rest of the queue still moves.
      if (batch.count > 1) {
        maxEvents = 1;
        continue;
      }
      if (skipCount < 4) {
        skip[skipCount++] = batch.items[0].offset;
        continue;
      }
      recordLocalError(SyncStage::Drain, "out_of_memory", report);
      return false;
    }

    JsonWriter w(body, cap);
    w.objectBegin();
    w.kvInt("protocol_version", 2);
    w.kvSeq("cursor", s.cursor);
    w.kvInt("pull_limit", config_.pullLimit);
    w.key("events");
    w.arrayBegin();
    Outbox::EnvelopeResult envelope = Outbox::EnvelopeResult::Ok;
    for (uint8_t i = 0; i < batch.count && envelope == Outbox::EnvelopeResult::Ok; ++i) {
      envelope = outbox_.writeEnvelope(batch.items[i], w);
    }
    if (envelope == Outbox::EnvelopeResult::Quarantined) {
      // It went bad since the scan and left the queue. Select again.
      delete[] body;
      ++report.eventsQuarantined;
      continue;
    }
    if (envelope == Outbox::EnvelopeResult::IoError) {
      delete[] body;
      recordLocalError(SyncStage::Drain, "outbox_read", report);
      return false;
    }
    w.arrayEnd();
    writeClient(w);
    w.objectEnd();
    if (!w.ok()) {
      delete[] body;
      recordLocalError(SyncStage::Drain, "body_overflow", report);
      return false;
    }

    // Persisted before any network IO: from here the event is immutable, and a lost
    // reply retries this same envelope.
    if (!outbox_.markAttempted(batch)) {
      delete[] body;
      recordLocalError(SyncStage::Drain, "outbox_write", report);
      return false;
    }
    const uint64_t requested = s.cursor;
    const ApiResult r = api_.call("POST", kExchangePath, reinterpret_cast<const uint8_t*>(body), w.size());
    delete[] body;
    report.eventsSent = static_cast<uint16_t>(report.eventsSent + batch.count);

    if (r.kind == ApiKind::HttpError) {
      if (isSequenceConflict(r)) {
        if (repairUsed || !repairSequences(batch, report)) {
          recordFailure(SyncStage::Drain, r, report);
          return false;
        }
        repairUsed = true;
        continue;
      }
      if (isCursorRecoverable(r) && !cursorRecovered && recoverCursor(r, report)) {
        cursorRecovered = true;
        continue;
      }
      if (isPoisonAnswer(r, fs_)) {
        if (batch.count > 1) {
          maxEvents = 1;
          continue;
        }
        if (report.eventsQuarantined >= kMaxQuarantinesPerSession) {
          recordFailure(SyncStage::Drain, r, report);
          return false;
        }
        if (!outbox_.quarantine(batch.items[0].offset)) {
          recordLocalError(SyncStage::Drain, "outbox_write", report);
          return false;
        }
        ++report.eventsQuarantined;
        ++s.rejectedEvents;
        copyText(s.lastRejection, sizeof(s.lastRejection), r.code[0] ? r.code : "request_refused");
        setDiag(SyncStage::Drain, s.lastRejection, r.message, r);
        continue;
      }
    }
    if (r.kind != ApiKind::Ok) {
      recordFailure(SyncStage::Drain, r, report);
      return false;
    }

    File f(fs_, kSpoolPath, IFs::Mode::Read);
    if (!f) {
      recordLocalError(SyncStage::Drain, "spool_read", report);
      return false;
    }
    PageInfo page;
    if (!scanPage(f, page) || !page.hasEvents || !page.hasCursor || page.cursor < requested) {
      recordProtocolError(SyncStage::Drain, "malformed_response", r, report);
      return false;
    }
    noteServerTime(page);
    // Cursor first, ack second: a crash in between resends the same ids, which the
    // server answers from its record, instead of losing a page.
    if (!applyPage(f, page, SyncStage::Drain, r, report)) return false;
    s.committedCursor = requested;
    if (!processAcks(f, page, batch, r, report)) return false;
  }
}

// ---------------------------------------------------------------------------------
// Housekeeping at the end of a session. Best effort.
// ---------------------------------------------------------------------------------

void SyncEngine::resolveSuggestion(const BookId& book) {
  BookLedger& ledger = scratch_->ledger;
  ledgerLoaded_ = false;
  if (!store_.loadBook(book, ledger)) return;
  if (ledger.suggestionAction == SuggestionAction::None || !ledger.suggestionId[0]) return;
  if (!api_.canContinue()) return;
  char path[96];
  JsonWriter p(path, sizeof(path));
  p.raw("/api/sync/v2/suggestions/");
  p.raw(ledger.suggestionId);
  p.raw(ledger.suggestionAction == SuggestionAction::Accept ? "/accept" : "/dismiss");
  if (!p.ok() || p.size() >= sizeof(path)) return;
  path[p.size()] = '\0';
  static constexpr char kEmpty[] = "{}";
  const ApiResult r = api_.call("POST", path, reinterpret_cast<const uint8_t*>(kEmpty), 2);
  // 404 and 409: it expired or was resolved elsewhere. Either way it is settled.
  if (r.kind == ApiKind::Ok || (r.kind == ApiKind::HttpError && (r.status == 404 || r.status == 409))) {
    ledger.suggestionId[0] = '\0';
    ledger.suggestionAction = SuggestionAction::None;
    store_.saveBook(ledger);
  }
}

void SyncEngine::reportHealth(const SessionReport& report) {
  const bool reachedServer = report.outcome == SessionOutcome::Ok || report.outcome == SessionOutcome::ServerError ||
                             report.outcome == SessionOutcome::ProtocolError;
  if (!reachedServer || !api_.canContinue()) return;
  char body[192];
  JsonWriter w(body, sizeof(body));
  w.objectBegin();
  const Diagnostics& d = store_.state().diag;
  if (report.outcome == SessionOutcome::Ok) {
    w.kv("outcome", "success");
  } else {
    w.kv("outcome", "error");
    w.kv("error_code", d.code[0] ? d.code : "unknown");
  }
  w.kvInt("queue_depth", static_cast<int64_t>(outbox_.depth()));
  w.objectEnd();
  if (!w.ok()) return;
  api_.call("POST", kHealthPath, reinterpret_cast<const uint8_t*>(body), w.size());
}

void SyncEngine::maybeRotate(SessionReport& report) {
  Credentials& c = store_.creds();
  const WallTime now = clock_.wallNow();
  if (now.precision < TimePrecision::Anchored) return;
  const bool byRenew = c.renewAfterEpoch != 0 && now.epochSec >= c.renewAfterEpoch;
  const bool byExpiry = c.expiresAtEpoch != 0 && now.epochSec + kRotateMarginSec >= c.expiresAtEpoch;
  if ((!byRenew && !byExpiry) || !api_.canContinue()) return;

  static constexpr char kEmpty[] = "{}";
  const ApiResult r = api_.call("POST", kRotatePath, reinterpret_cast<const uint8_t*>(kEmpty), 2);
  if (r.kind != ApiKind::Ok) {
    scrubFile(fs_, kSpoolPath);  // A cut answer can still hold the token.
    if (r.kind == ApiKind::HttpError && (r.status == 401 || r.status == 403)) recordFailure(SyncStage::Rotate, r, report);
    return;
  }
  File f(fs_, kSpoolPath, IFs::Mode::Read);
  if (!f) return;
  FileByteSource source(f);
  JsonScanner scan(source);
  char key[24];
  char token[sizeof(c.token)] = {0};
  bool truncated = false;
  int64_t expires = 0;
  if (scan.enterObject()) {
    while (scan.nextMember(key, sizeof(key))) {
      if (strcmp(key, "credential") == 0 && scan.enterObject()) {
        while (scan.nextMember(key, sizeof(key))) {
          if (strcmp(key, "token") == 0) {
            scan.readString(token, sizeof(token), &truncated);
          } else if (strcmp(key, "expires_at") == 0) {
            char text[40];
            if (!scan.readString(text, sizeof(text)) || !parseRfc3339(text, expires)) expires = 0;
          } else {
            scan.skipValue();
          }
        }
      } else {
        scan.skipValue();
      }
    }
  }
  f.close();
  scrubFile(fs_, kSpoolPath);
  if (scan.failed() || !token[0] || truncated) return;
  copyText(c.token, sizeof(c.token), token);
  memset(token, 0, sizeof(token));
  c.expiresAtEpoch = expires;
  c.renewAfterEpoch = 0;
  store_.saveCredentials();
}

DisconnectResult SyncEngine::disconnect(bool online) {
  bool invalidated = false;
  SessionReport ignored;
  if (online && startSession(SessionKind::Manual, ignored)) {
    api_.beginSession(kDisconnectBudgetMs);
    char body[64];
    JsonWriter w(body, sizeof(body));
    w.objectBegin();
    w.kvInt("expires_in_seconds", kShortestCredentialTtlSec);
    w.kvInt("grace_seconds", 0);
    w.objectEnd();
    const ApiResult r = api_.call("POST", kRotatePath, reinterpret_cast<const uint8_t*>(body), w.size());
    // Already refused is just as dead.
    invalidated = r.kind == ApiKind::Ok || (r.kind == ApiKind::HttpError && (r.status == 401 || r.status == 403));
    scrubFile(fs_, kSpoolPath);
    api_.endSession();
  }
  if (!store_.forgetCredential()) return DisconnectResult::LocalError;
  return invalidated ? DisconnectResult::ServerInvalidated : DisconnectResult::StillValidOnServer;
}

// ---------------------------------------------------------------------------------
// Finalize
// ---------------------------------------------------------------------------------

bool SyncEngine::appendEvent(EventKind kind, const BookId& book, const char* body, size_t len, bool replace) {
  return outbox_.append(kind, book.md5, body, len, replace, nullptr, nullptr) == Outbox::AppendResult::Ok;
}

bool SyncEngine::enqueueProgress(const BookId& book, const EventTime& time, const Position& position, bool explicitJump) {
  // A place this device does not know is not a place to publish: percentage 0 would
  // become the head and send the other readers to the first page.
  if (!position.hasPct) return true;
  JsonWriter counter(nullptr, 0);
  events::writeProgress(counter, book, time, position, explicitJump);
  char* body = new (std::nothrow) char[counter.size()];
  if (!body) return false;
  JsonWriter w(body, counter.size());
  events::writeProgress(w, book, time, position, explicitJump);
  // Rule 5: one unattempted progress event per book, replaced until it is attempted.
  const bool ok = appendEvent(EventKind::Progress, book, body, w.size(), true);
  delete[] body;
  return ok;
}

// A long uninterrupted reading is not an invalid one: as session.lua:finishAll, it is split
// into segments the server accepts, with the pages counted on the last. False: something
// valid is not in the queue, and the caller has to keep it for the next finalize.
bool SyncEngine::enqueueSession(const BookId& book, const SessionSummary& session, FinalizeResult& result) {
  int64_t start = session.started.epochSec;
  const int64_t end = session.ended.epochSec;
  bool queued = true;
  bool any = false;
  while (queued && end - start >= events::kMinSessionSec) {
    const int64_t segmentEnd = end - start > events::kMaxSessionSec ? start + events::kMaxSessionSec : end;
    SessionSummary segment = session;
    segment.started.epochSec = start;
    segment.ended.epochSec = segmentEnd;
    segment.pagesRead = segmentEnd == end ? session.pagesRead : 0;
    segment.startPage = start == session.started.epochSec ? session.startPage : session.endPage;
    JsonWriter counter(nullptr, 0);
    if (!events::writeSession(counter, book, segment)) return true;  // The clock: nothing honest to send.
    char* body = new (std::nothrow) char[counter.size()];
    queued = body != nullptr;
    if (body) {
      JsonWriter w(body, counter.size());
      events::writeSession(w, book, segment);
      queued = appendEvent(EventKind::Session, book, body, w.size(), false);
      delete[] body;
    }
    any = any || queued;
    start = segmentEnd;
  }
  result.sessionQueued = any && queued;
  return queued;
}

FinalizeResult SyncEngine::finalizeBook(const FinalizeInput& input) {
  FinalizeResult result;
  if (!input.book.id.valid() || !store_.creds().paired()) {
    result.ok = true;  // Nothing to publish to. Not a failure.
    return result;
  }
  if ((!scratch_ && !begin()) || (!outbox_.isOpen() && !outbox_.open())) return result;

  BookLedger& ledger = scratch_->ledger;
  ledgerLoaded_ = false;
  ledgerDirty_ = false;
  if (!store_.loadBook(input.book.id, ledger)) return result;

  EventTime time;
  time.wall = input.lastTurn;
  time.hasMonotonic = input.hasLastTurnMonotonic;
  time.monotonicMs = input.lastTurnMonotonicMs;

  // A second finalize of the same reading (exit, then sleep) is not a new event.
  const bool samePlace = ledger.hasFinalized && ledger.lastFinalized.pctMicro == input.position.pctMicro &&
                         strcmp(ledger.lastFinalized.xpointer, input.position.xpointer) == 0 &&
                         ledger.lastTurnEpoch == input.lastTurn.epochSec;
  bool ok = true;
  if (input.paged && input.position.hasPct && !samePlace) {
    if (enqueueProgress(input.book.id, time, input.position, false)) {
      result.progressQueued = true;
      ledger.lastFinalized = input.position;
      ledger.hasFinalized = true;
      ledger.lastTurnEpoch = input.lastTurn.epochSec;
      if (!store_.saveBook(ledger)) ok = false;
      // Something new was read: if the book is held, that is worth asking about once more.
      if (store_.state().holds.clearChecked(input.book.id.md5) && !store_.saveState()) ok = false;
    } else {
      ok = false;
    }
  }

  if (input.session && !enqueueSession(input.book.id, *input.session, result)) ok = false;

  if (input.clips && !finalizeClips(input, time, result)) ok = false;
  result.ok = ok;
  return result;
}

bool SyncEngine::finalizeClips(const FinalizeInput& input, const EventTime& time, FinalizeResult& result) {
  size_t clipCount = 0;
  if (!input.clips->count(clipCount)) return false;  // Unreadable is not "all deleted".
  if (clipCount > Store::kMaxAnnotations) {
    // The ledger has no room to know the rest, so nothing can be concluded from their
    // absence: pending keeps the deletion pass below from running on a partial read.
    clipCount = Store::kMaxAnnotations;
    result.annotationsPending = true;
  }

  AnnotationRecord* stored = nullptr;
  uint16_t storedCount = 0;
  if (!store_.loadAnnotations(input.book.id, &stored, &storedCount)) return false;

  // Working copy: what the ledger will say. Bit 0 of `bookmark` marks "seen on disk".
  const size_t room = static_cast<size_t>(storedCount) + clipCount;
  AnnotationRecord* next = new (std::nothrow) AnnotationRecord[room ? room : 1];
  uint8_t* seen = new (std::nothrow) uint8_t[storedCount ? storedCount : 1];
  if (!next || !seen) {
    delete[] stored;
    delete[] next;
    delete[] seen;
    return false;
  }
  memset(seen, 0, storedCount ? storedCount : 1);
  if (storedCount) memcpy(next, stored, sizeof(AnnotationRecord) * storedCount);
  uint16_t nextCount = storedCount;
  bool ok = true;
  bool changed = false;
  uint16_t published = 0;

  for (size_t i = 0; i < clipCount; ++i) {
    ClipKey key;
    if (!input.clips->key(i, key)) {
      ok = false;
      break;
    }
    const uint64_t ledgerKey = events::clipLedgerKey(input.book.id, key);
    bool known = false;
    for (uint16_t j = 0; j < storedCount && !known; ++j) {
      if (stored[j].key == ledgerKey) {
        seen[j] = 1;
        known = true;
      }
    }
    for (uint16_t j = storedCount; j < nextCount && !known; ++j) known = next[j].key == ledgerKey;
    if (known) continue;
    if (published >= config_.maxAnnotationEvents || nextCount >= Store::kMaxAnnotations) {
      result.annotationsPending = true;
      continue;
    }
    ClipDetail detail;
    if (!input.clips->detail(i, detail)) {
      ok = false;
      break;
    }
    uint8_t syncId[16];
    events::deriveSyncId(input.book.id, key, syncId);
    JsonWriter counter(nullptr, 0);
    if (!events::writeAnnotation(counter, input.book, time, key, detail, syncId)) {
      // It can never be valid (no text or no locator). Record it so it stops coming back.
      next[nextCount].key = ledgerKey;
      memcpy(next[nextCount].syncId, syncId, 16);
      next[nextCount].bookmark = key.bookmark ? 1 : 0;
      next[nextCount].revision = 0;
      ++nextCount;
      changed = true;
      continue;
    }
    char* body = new (std::nothrow) char[counter.size()];
    if (!body) {
      ok = false;
      break;
    }
    JsonWriter w(body, counter.size());
    events::writeAnnotation(w, input.book, time, key, detail, syncId);
    const bool queued = appendEvent(EventKind::Annotation, input.book.id, body, w.size(), false);
    delete[] body;
    if (!queued) {
      ok = false;
      break;
    }
    next[nextCount].key = ledgerKey;
    memcpy(next[nextCount].syncId, syncId, 16);
    next[nextCount].bookmark = key.bookmark ? 1 : 0;
    next[nextCount].revision = 0;
    ++nextCount;
    ++published;
    ++result.annotationsCreated;
    changed = true;
  }

  // Deletions only from a complete read of the clippings.
  if (ok && !result.annotationsPending) {
    uint16_t kept = 0;
    for (uint16_t j = 0; j < nextCount; ++j) {
      const bool wasStored = j < storedCount;
      if (wasStored && !seen[j]) {
        if (published >= config_.maxAnnotationEvents) {
          result.annotationsPending = true;
          next[kept++] = next[j];
          continue;
        }
        JsonWriter counter(nullptr, 0);
        const bool bookmark = next[j].bookmark != 0;
        events::writeAnnotationDeleted(counter, input.book, time, bookmark, next[j].syncId, next[j].revision);
        char* body = new (std::nothrow) char[counter.size()];
        if (!body) {
          ok = false;
          next[kept++] = next[j];
          continue;
        }
        JsonWriter w(body, counter.size());
        events::writeAnnotationDeleted(w, input.book, time, bookmark, next[j].syncId, next[j].revision);
        const bool queued = appendEvent(EventKind::Annotation, input.book.id, body, w.size(), false);
        delete[] body;
        if (!queued) {
          ok = false;
          next[kept++] = next[j];
          continue;
        }
        ++published;
        ++result.annotationsDeleted;
        changed = true;
        continue;
      }
      next[kept++] = next[j];
    }
    nextCount = kept;
  }

  if (changed && !store_.saveAnnotations(input.book.id, next, nextCount)) ok = false;
  delete[] stored;
  delete[] next;
  delete[] seen;
  return ok;
}

// ---------------------------------------------------------------------------------
// Small queries
// ---------------------------------------------------------------------------------

bool SyncEngine::hasUndo(const BookId& book) {
  if (!scratch_ && !begin()) return false;
  ledgerLoaded_ = false;
  return store_.loadBook(book, scratch_->ledger) && scratch_->ledger.hasUndo;
}

bool SyncEngine::takeUndo(const BookId& book, Position& out) {
  if (!hasUndo(book)) return false;
  out = scratch_->ledger.undo;
  scratch_->ledger.hasUndo = false;
  return store_.saveBook(scratch_->ledger);
}

bool SyncEngine::lastFinalized(const BookId& book, Position& out) {
  if (!scratch_ && !begin()) return false;
  ledgerLoaded_ = false;
  if (!store_.loadBook(book, scratch_->ledger) || !scratch_->ledger.hasFinalized) return false;
  out = scratch_->ledger.lastFinalized;
  return true;
}

bool SyncEngine::remoteHead(const BookId& book, RemoteHead& out) {
  if (!scratch_ && !begin()) return false;
  ledgerLoaded_ = false;
  if (!store_.loadBook(book, scratch_->ledger) || !scratch_->ledger.head.valid) return false;
  out = scratch_->ledger.head;
  return true;
}

EngineStatus SyncEngine::status() const {
  EngineStatus st;
  const SyncState& s = store_.state();
  st.paired = store_.creds().paired();
  st.needsRelink = s.needsRelink;
  st.depth = outbox_.depth();
  st.held = st.depth - outbox_.sendableDepth(&s.holds);
  st.cursor = s.cursor;
  st.lastSuccessEpoch = s.lastSuccessEpoch;
  st.nextAttemptEpoch = s.nextAttemptEpoch;
  st.skippedEvents = s.skippedEvents;
  st.rejectedEvents = s.rejectedEvents;
  st.quarantinedRecords = outbox_.quarantinedRecords();
  st.diag = s.diag;
  return st;
}

}  // namespace borges
