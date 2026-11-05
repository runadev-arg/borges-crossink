#pragma once

// Wire shape of events, in both directions.
//
// Outbound: writeProgress/writeAnnotation/writeAnnotationDeleted/writeSession write
// everything in an envelope except client_event_id and client_sequence, which the
// outbox owns. Pass a JsonWriter with a null buffer to measure first. Each returns
// false, writing nothing useful, if the event cannot be valid on the server.
//
// Inbound: parseRemoteEvent walks one element of `events` (or a `focus_event`) and
// keeps only the fields below, in fixed buffers. Oversized fields come out truncated
// and flagged, and unknown fields cost nothing.

#include <cstddef>
#include <cstdint>

#include "JsonStream.h"
#include "PageStats.h"
#include "Ports.h"
#include "Types.h"

namespace borges {

// Reference to the book being finalized or opened. Strings may be null.
struct BookRef {
  BookId id;
  const char* title = nullptr;
  const char* author = nullptr;
  const char* file = nullptr;
  int32_t totalPages = -1;
};

struct EventTime {
  WallTime wall;
  bool hasMonotonic = false;
  uint64_t monotonicMs = 0;
};

// One clipping or bookmark as CrossInk has it on disk, plus what the reader knows
// while the book is loaded.
struct ClipKey {
  uint32_t spine = 0;
  uint32_t textOffset = 0;
  uint64_t textHash = 0;
  bool bookmark = false;
};

struct ClipDetail {
  const char* text = nullptr;  // Required by the server.
  const char* note = nullptr;
  const char* chapter = nullptr;
  const char* color = nullptr;     // Defaults to gray.
  const char* xpointer = nullptr;     // From ChapterXPathResolver. Start of the selection.
  const char* xpointerEnd = nullptr;  // Its end. KOReader cannot place a highlight without it.
  int32_t page = -1;                  // Book-wide reference page. -1: worked out from pctMicro.
  bool hasPct = false;
  uint32_t pctMicro = 0;
  int64_t timeMs = 0;  // Wall time at save. 0 omits the field: clip timestamps are uptime.
};

struct SessionSummary {
  WallTime started;
  WallTime ended;
  int32_t pagesRead = 0;
  int32_t startPage = -1;
  int32_t endPage = -1;
};

namespace events {

constexpr int32_t kMinSessionSec = 60;
constexpr int32_t kMaxSessionSec = 14400;

// position must have a percentage: the server requires one, and there is no honest default.
void writeProgress(JsonWriter& w, const BookId& book, const EventTime& time, const Position& position,
                   bool explicitJump);

bool writeAnnotation(JsonWriter& w, const BookRef& book, const EventTime& time, const ClipKey& key,
                     const ClipDetail& detail, const uint8_t syncId[16]);

// baseRevision: the revision this device last saw of it, so that deleting a highlight that
// was edited on the web is a delete and not a conflict.
void writeAnnotationDeleted(JsonWriter& w, const BookRef& book, const EventTime& time, bool bookmark,
                            const uint8_t syncId[16], uint32_t baseRevision);

// On the same book-wide scale as progress: page numbers of one device mean nothing on another.
int32_t referencePage(uint32_t pctMicro, int32_t totalPages);

bool writeSession(JsonWriter& w, const BookId& book, const SessionSummary& session);

// One self-contained chunk of a book's page rows. The books[] entry is also what creates a
// document for a book with no highlights, so it goes in every chunk, as in statsync.lua.
constexpr uint8_t kPageStatRowsPerEvent = 32;
void writePageStats(JsonWriter& w, const BookRef& book, TimePrecision precision, const PageStatRow* rows, uint8_t count,
                    uint8_t chunkIndex);

uint64_t clipLedgerKey(const BookId& book, const ClipKey& clip);
void deriveSyncId(const BookId& book, const ClipKey& clip, uint8_t out[16]);

}  // namespace events

struct RemoteEvent {
  char eventId[72] = {0};  // "progress:<uuid>:<revision>" must fit.
  bool hasEventId = false;
  bool hasSeq = false;
  uint64_t serverSeq = 0;
  char eventType[32] = {0};
  char bookKind[24] = {0};
  char bookValue[40] = {0};
  char originId[40] = {0};
  char originName[48] = {0};
  char occurredAt[32] = {0};
  TimePrecision precision = TimePrecision::Unknown;
  bool hasPct = false;
  uint32_t pctMicro = 0;
  int32_t page = -1;
  int32_t totalPages = -1;
  bool explicitJump = false;
  bool readingHead = false;
  bool xpointerUsable = false;  // Present and it fit: a cut xpointer is no xpointer.
  char xpointer[kMaxXpointer] = {0};
  char directive[24] = {0};
  char suggestionId[40] = {0};
  // Annotations only.
  char syncId[40] = {0};
  uint64_t annotationRevision = 0;  // 0: the server did not say.

  bool isProgress() const;
  bool isAnnotation() const;
  bool isAnnotationDelete() const;
};

// Consumes exactly one JSON value. A non-object value yields an empty event. Returns
// false only if the document is malformed.
bool parseRemoteEvent(JsonScanner& scan, RemoteEvent& out);

}  // namespace borges
