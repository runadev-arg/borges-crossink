#pragma once

// One Borges sync session: focus check, pull pages, drain the outbox, report.
//
// MEMORY BUDGET (ESP32-C3, about 100 KB free heap in a network boot, 45-50 KB with a
// book loaded, TLS needs 35 KB of that).
//
//   Heap, whole engine lifetime
//     Scratch (one RemoteEvent, BookLedger, Batch)                      3.5 KB
//     Outbox index                  24 B per live event, 128 at most    ~3.1 KB
//   Heap, one request at a time
//     Exchange body: default bound 8 KiB + one envelope. A single oversized event
//     goes alone, so the worst case is that event (a queued event is under 48 KiB).
//     If that allocation fails, the event waits and the rest of the queue moves on.
//   Stack: 2 KB at most in any one frame (measured with -fstack-usage: Catalog::fetchPage,
//   PairingFlow::requestCode, Store::begin; the frames of the sync path are smaller).
//
// Not budgeted because it cannot grow: responses. They are spooled to SD and walked
// with a 192-byte window, so memory use is independent of what the server sends, and
// nothing recurses on server input. Fixed buffers truncate oversized fields; the parse
// budget (default 12 KiB per pulled event) only decides which events are skipped.
//
// The engine does not know about Wi-Fi. The glue checks gate(), connects, calls one of
// the run*() functions, and turns the radio off afterwards on every path. All decisions
// are deterministic given IClock and IRandom.

#include <cstddef>
#include <cstdint>

#include "Api.h"
#include "Events.h"
#include "Outbox.h"
#include "Ports.h"
#include "ResumePolicy.h"
#include "Store.h"
#include "Types.h"

namespace borges {

struct EngineConfig {
  const char* firmwareVersion = "";
  const char* clientVersion = "";  // crossink-borges/<ver>
  uint32_t maxBodyBytes = 8 * 1024;
  uint8_t maxEventsPerBatch = 20;
  uint8_t maxPullPages = 4;
  uint8_t pullLimit = 20;
  uint8_t maxHeldChecks = 3;         // Focus checks for held books per session.
  uint8_t maxAnnotationEvents = 32;  // Queued per finalize; the rest wait for the next one.
  uint32_t eventParseBudget = 12 * 1024;
  uint32_t openBudgetMs = 12000;
  uint32_t sleepBudgetMs = 15000;
  uint32_t manualBudgetMs = 120000;
};

enum class SessionKind : uint8_t { Open, Sleep, Manual };

enum class Gate : uint8_t {
  Go,
  NotPaired,
  NeedsRelink,  // Automatic sessions stop. Show "Volve a conectar". Manual still tries.
  BackedOff,    // Automatic sessions wait. Manual always tries.
  NothingToDo,  // A sleep session with an empty queue costs no radio.
};

enum class SessionOutcome : uint8_t {
  Ok,
  AwaitingChoice,  // runOpen/runManual: resolve with applyChoice() or abandonChoice().
  NotPaired,
  Cancelled,
  BudgetExhausted,
  NeedsRelink,
  RateLimited,
  Transport,
  ServerError,
  ProtocolError,  // Malformed response, unknown version, or an ack that does not match.
  LocalError,     // SD or memory.
  StaleChoice,    // applyChoice() without a matching pending decision.
};

struct SessionReport {
  SessionOutcome outcome = SessionOutcome::Ok;
  bool focusChecked = false;
  bool focusInvalid = false;
  bool pullFailed = false;
  bool drainBlockedByPull = false;
  bool cursorRecovered = false;
  bool sequenceRepaired = false;
  uint8_t pagesPulled = 0;
  uint16_t eventsApplied = 0;
  uint16_t eventsSkipped = 0;  // Over the parse budget. The cursor still advanced.
  uint16_t eventsSent = 0;     // Sent in exchanges, retries included.
  uint16_t eventsAcked = 0;
  uint16_t eventsRejected = 0;
  uint16_t eventsQuarantined = 0;
  size_t depth = 0;  // Left in the outbox.
  size_t held = 0;   // Of those, held back by an unresolved check.
};

// What the prompt needs. No remote page number on purpose: it means nothing on this
// device, so a screen must go through percentage.
struct ResumeDecision {
  bool ask = false;
  resume::Verdict verdict = resume::Verdict::NoEvent;
  BookId book;
  Position local;
  bool hasRemotePct = false;
  uint32_t remotePct = 0;
  bool xpointerUsable = false;
  char remoteXpointer[kMaxXpointer] = {0};
  char remoteDeviceName[48] = {0};
  char occurredAt[32] = {0};  // Empty unless showDate.
  bool showDate = false;
  uint8_t warnings = 0;
  uint64_t fingerprint = 0;
};

enum class ResumeChoice : uint8_t { GoThere, StayHere };

// The book's clippings and bookmarks, as they are on disk. count() returning false
// means "could not read": nothing is published and, above all, nothing is deleted.
class IClipSource {
 public:
  virtual ~IClipSource() = default;
  virtual bool count(size_t& out) = 0;
  virtual bool key(size_t index, ClipKey& out) = 0;
  virtual bool detail(size_t index, ClipDetail& out) = 0;
};

// Everything known about a book when it is closed, or the device goes to sleep with it.
struct FinalizeInput {
  BookRef book;
  Position position;
  bool paged = false;  // A real page turn happened since it was last finalized.
  WallTime lastTurn;   // Time of the last real page turn.
  bool hasLastTurnMonotonic = false;
  uint64_t lastTurnMonotonicMs = 0;
  const SessionSummary* session = nullptr;
  IClipSource* clips = nullptr;
  // Rows that reach the queue leave the buffer, so it can stay in the reader across finalizes.
  PageStatBuffer* pageStats = nullptr;
};

struct FinalizeResult {
  bool ok = false;  // False: try again at the next finalize, nothing was lost.
  bool progressQueued = false;
  bool sessionQueued = false;
  uint16_t annotationsCreated = 0;
  bool sessionFailed = false;  // There was a valid session and it did not reach the queue.
  uint16_t annotationsDeleted = 0;
  bool annotationsPending = false;  // Limit reached; the rest go at the next finalize.
  uint8_t pageStatEvents = 0;
};

enum class DisconnectResult : uint8_t {
  ServerInvalidated,   // The server confirmed that the token no longer works.
  StillValidOnServer,  // Forgotten here. The server still honours it: say so on the screen.
  LocalError,          // Could not write to the card: nothing changed.
};

struct EngineStatus {
  bool paired = false;
  bool needsRelink = false;
  size_t depth = 0;
  size_t held = 0;
  uint64_t cursor = 0;
  int64_t lastSuccessEpoch = 0;
  int64_t nextAttemptEpoch = 0;
  uint32_t skippedEvents = 0;
  uint32_t rejectedEvents = 0;
  uint32_t quarantinedRecords = 0;
  Diagnostics diag;
};

class SyncEngine {
 public:
  SyncEngine(Store& store, Outbox& outbox, IFs& fs, IHttp& http, IClock& clock, IRandom& random, ICancel& cancel,
             const EngineConfig& config);
  ~SyncEngine();
  SyncEngine(const SyncEngine&) = delete;
  SyncEngine& operator=(const SyncEngine&) = delete;

  // Allocates scratch and opens the outbox. The store must have begun.
  bool begin();
  void end();

  // No IO. For a screen to show. Call before bringing the radio up.
  Gate gate(SessionKind kind) const;
  // gate(), when this answer decides whether an automatic session runs: a skip for link
  // failures is spent here, and saved.
  Gate decideAutomatic(SessionKind kind);
  // An automatic session could not get a link. A network out of reach costs seconds of radio
  // at every open and sleep and heals by itself, so after two in a row the next five automatic
  // attempts are skipped. Counted in attempts, because the clock may be what is missing. Any
  // session that gets a link clears it. Manual sessions neither count nor care.
  void noteLinkFailure();

  // Local, no network. Holds this book's progress until a check resolves it. Must run
  // whenever a book opens, including when no session will run.
  bool onBookOpened(const BookId& book);

  // Local, no network. Materializes progress, the session and clipping changes.
  FinalizeResult finalizeBook(const FinalizeInput& input);

  // Session entry points. Each owns the budget for its kind.
  // Local, no network. A book first opened under this account after it took over the card: what is
  // there now was made under another one, so it is recorded as seen and never sent. Called when
  // the book opens, before anything can be highlighted, so what comes later is this account's.
  // No-op when the ledger is this account's. False: could not read or write, try at the finalize.
  bool seedClipsIfForeign(const BookId& book, IClipSource& clips);

  // "Search at open" went off: nothing will ever answer these, so they stop waiting.
  void releaseHolds();

  SessionReport runOpenSession(const BookRef& book, const Position& local, ResumeDecision& out);
  SessionReport runSleepSession();
  // Asked for from the drawer. An offer of another device's place, answered like any prompt:
  // AwaitingChoice with out filled, or Ok with out.ask false when no other device has one.
  SessionReport runPositionsSession(const BookRef& book, const Position& local, ResumeDecision& out);
  // book, local and out may all be null: then no prompt can come out of it.
  SessionReport runManualSession(const BookRef* book, const Position* local, ResumeDecision* out);

  // Resolves an AwaitingChoice. For GoThere: before is where the reader was (it becomes
  // the undo point) and after is where the glue already navigated to. For StayHere only
  // before matters. online=false does the durable part only.
  SessionReport applyChoice(const ResumeDecision& decision, ResumeChoice choice, const Position& before,
                            const Position& after, bool online = true);
  // The reader pressed something else. The book stays held and will ask again.
  SessionReport abandonChoice();

  // "Volver a donde estaba". Single use.
  bool hasUndo(const BookId& book);
  bool takeUndo(const BookId& book, Position& out);

  // Where this device last published itself in this book, xpointer included.
  bool lastFinalized(const BookId& book, Position& out);

  // Latest position another device was seen at, from replayed pulls.
  bool remoteHead(const BookId& book, RemoteHead& out);

  // Forgets this device's credential. There is no revoke route a device may call, so when
  // online this rotates it into one that expires in a minute, with no grace for the old
  // one, and throws the new one away: the old token dies at once and the new one is known
  // to nobody. The queue stays: re-pairing the same account finds it.
  DisconnectResult disconnect(bool online);

  EngineStatus status() const;
  const Diagnostics& diagnostics() const { return store_.state().diag; }

 private:
  struct Scratch;
  struct PageInfo;

  bool hasSleepWork() const;
  bool startSession(SessionKind kind, SessionReport& report);
  SessionReport finish(SessionReport report, SessionKind kind);
  void pullAndDrain(SessionReport& report);

  void recordFailure(SyncStage stage, const ApiResult& result, SessionReport& report);
  void recordProtocolError(SyncStage stage, const char* code, const ApiResult& result, SessionReport& report);
  void recordLocalError(SyncStage stage, const char* code, SessionReport& report);
  void noteInterruption(SessionReport& report);
  void setDiag(SyncStage stage, const char* code, const char* detail, const ApiResult& result);

  // focus() returns false when the session must stop.
  bool focus(const BookId& book, const Position* local, ResumeDecision* out, SessionReport& report);
  bool checkHeldBooks(const BookId* except, SessionReport& report);
  bool pullPages(SessionReport& report);
  bool drain(SessionReport& report);
  bool ensureCapabilities(SessionReport& report);
  void commitCursor(SessionReport& report);
  void writeClient(JsonWriter& w) const;
  void reportHealth(const SessionReport& report);
  void maybeRotate(SessionReport& report);
  void resolveSuggestion(const BookId& book);

  bool scanPage(File& f, PageInfo& page);
  // Applies a page's events and persists the cursor. Returns false when the session must stop.
  bool applyPage(File& f, const PageInfo& page, SyncStage stage, const ApiResult& result, SessionReport& report);
  bool applyRemote(SessionReport& report);
  bool flushLedger();
  bool processAcks(File& f, const PageInfo& page, const Batch& batch, const ApiResult& result, SessionReport& report);
  bool repairSequences(const Batch& batch, SessionReport& report);
  bool recoverCursor(const ApiResult& result, SessionReport& report);
  void noteServerTime(const PageInfo& page);

  bool enqueueProgress(const BookId& book, const EventTime& time, const Position& position, bool explicitJump);
  bool enqueueSession(const BookId& book, const SessionSummary& session, FinalizeResult& result);
  bool enqueuePageStats(const FinalizeInput& input, FinalizeResult& result);
  bool finalizeClips(const FinalizeInput& input, const EventTime& time, FinalizeResult& result);
  bool appendEvent(EventKind kind, const BookId& book, const char* body, size_t len, bool replace);

  Store& store_;
  Outbox& outbox_;
  IFs& fs_;
  IClock& clock_;
  bool seedClips(const BookId& book, IClipSource& clips);
  IRandom& random_;
  Api api_;
  EngineConfig config_;
  Scratch* scratch_ = nullptr;
  bool ledgerDirty_ = false;
  bool ledgerLoaded_ = false;
  bool pendingChoice_ = false;
  uint64_t pendingFingerprint_ = 0;
  BookId pendingBook_;
  SessionKind currentKind_ = SessionKind::Manual;
};

}  // namespace borges
