#include "Events.h"

#include <cstring>

namespace borges {

namespace events {

namespace {

void writeHead(JsonWriter& w, const char* type, const char* aggregateType) {
  w.kvInt("event_version", 1);
  w.kv("event_type", type);
  w.kv("aggregate_type", aggregateType);
  w.key("aggregate_id");
}

void writeTail(JsonWriter& w, const BookId& book, const EventTime& time) {
  w.kvNull("document_id");
  w.key("book_identifier");
  w.objectBegin();
  w.kv("kind", "koreader_partial_md5");
  w.kv("value", book.hex);
  w.objectEnd();
  // An unknown clock says so: no invented date.
  if (time.wall.precision == TimePrecision::Unknown) {
    w.kvNull("occurred_at");
  } else {
    char stamp[21];
    formatRfc3339(time.wall.epochSec, stamp);
    w.kv("occurred_at", stamp);
  }
  w.kv("time_precision", precisionName(time.wall.precision));
  if (time.hasMonotonic) w.kvSeq("monotonic_ms", time.monotonicMs);
  w.key("payload");
}

void writeBook(JsonWriter& w, const BookRef& book) {
  // The server creates the document from this. A title is the one required field.
  w.key("book");
  w.objectBegin();
  w.kv("title", book.title && *book.title ? book.title : book.id.hex);
  if (book.author && *book.author) w.kv("author", book.author);
  if (book.file && *book.file) w.kv("file", book.file);
  if (book.totalPages >= 1) w.kvInt("total_pages", book.totalPages);
  w.objectEnd();
}

// As KOReader does before it has heard otherwise. Without it, highlighting a passage
// again after deleting it reads as a legacy client resurrecting a tombstone: the server
// opens a conflict and the entry stays gone. With it, the server follows this device's
// own chain of events, and still compares branches if another device touched it since.
void writeBaseRevision(JsonWriter& w) { w.kv("base_revision", "0"); }

constexpr size_t kMaxStatsText = 500;

bool hasNonSpace(const char* text) {
  if (!text) return false;
  for (const char* p = text; *p; ++p) {
    if (*p != ' ' && *p != '\n' && *p != '\t' && *p != '\r') return true;
  }
  return false;
}

}  // namespace

void writeProgress(JsonWriter& w, const BookId& book, const EventTime& time, const Position& position,
                   bool explicitJump) {
  char aggregate[21 + 33];
  memcpy(aggregate, "koreader_partial_md5:", 21);
  memcpy(aggregate + 21, book.hex, 33);
  writeHead(w, "progress.changed", "reading_progress");
  w.value(aggregate);
  writeTail(w, book, time);
  w.objectBegin();
  w.key("percentage");
  w.valuePercent(position.pctMicro > 100000000u ? 100000000u : position.pctMicro);
  // The server rejects current_page > total_pages, so an inconsistent pair is left out.
  const bool pagesValid = position.page >= 0 && position.totalPages >= 1 && position.page <= position.totalPages;
  if (pagesValid) {
    w.kvInt("current_page", position.page);
    w.kvInt("total_pages", position.totalPages);
  }
  if (position.xpointer[0]) w.kv("xpointer", position.xpointer);
  w.kvBool("explicit_jump", explicitJump);
  w.objectEnd();
}

int32_t referencePage(uint32_t pctMicro, int32_t totalPages) {
  if (totalPages < 1) return -1;
  if (pctMicro > 100000000u) pctMicro = 100000000u;
  const int64_t page = (static_cast<int64_t>(pctMicro) * totalPages + 50000000) / 100000000;
  return page < 1 ? 1 : static_cast<int32_t>(page);
}

bool writeAnnotation(JsonWriter& w, const BookRef& book, const EventTime& time, const ClipKey& key, const ClipDetail& d,
                     const uint8_t syncId[16]) {
  const int32_t page = d.page >= 0 ? d.page : d.hasPct ? referencePage(d.pctMicro, book.totalPages) : -1;
  // A bookmark has no selection to be without: annotationadapter.lua:annotationText.
  char bookmarkText[32];
  const char* text = d.text;
  if (!hasNonSpace(text)) {
    if (!key.bookmark) return false;
    JsonWriter label(bookmarkText, sizeof(bookmarkText) - 1);
    label.raw("Bookmark");
    if (page >= 0) {
      char number[21];
      formatU64(static_cast<uint64_t>(page), number);
      label.raw(" - page ");
      label.raw(number);
    }
    bookmarkText[label.size()] = '\0';
    text = bookmarkText;
  }
  const bool hasXp = d.xpointer && *d.xpointer;
  if (!hasXp && page < 0) return false;  // annotation_locator_required.
  writeHead(w, key.bookmark ? "bookmark.upserted" : "annotation.created", "annotation");
  w.valueUuid(syncId);
  writeTail(w, book.id, time);
  w.objectBegin();
  w.key("sync_id");
  w.valueUuid(syncId);
  w.kv("kind", key.bookmark ? "bookmark" : "highlight");
  w.key("text");
  w.valueNormalizedText(text);
  if (d.note && *d.note) w.kv("note", d.note);
  if (d.chapter && *d.chapter) w.kv("chapter", d.chapter);
  if (page >= 0) w.kvInt("page", page);
  if (book.totalPages >= 1) w.kvInt("total_pages", book.totalPages);
  if (d.timeMs > 0) w.kvInt("time", d.timeMs);
  w.kv("color", d.color && *d.color ? d.color : "gray");
  w.kv("drawer", key.bookmark ? "bookmark" : "lighten");
  if (hasXp) w.kv("xpointer", d.xpointer);
  if (hasXp && d.xpointerEnd && *d.xpointerEnd) {
    w.key("position");
    w.objectBegin();
    w.kv("pos0", d.xpointer);
    w.kv("pos1", d.xpointerEnd);
    w.objectEnd();
  }
  if (d.hasPct) {
    w.key("percentage");
    w.valuePercent(d.pctMicro > 100000000u ? 100000000u : d.pctMicro);
  }
  writeBaseRevision(w);
  writeBook(w, book);
  w.objectEnd();
  return true;
}

void writeAnnotationDeleted(JsonWriter& w, const BookRef& book, const EventTime& time, bool bookmark,
                            const uint8_t syncId[16], uint32_t baseRevision) {
  writeHead(w, bookmark ? "bookmark.deleted" : "annotation.deleted", "annotation");
  w.valueUuid(syncId);
  writeTail(w, book.id, time);
  w.objectBegin();
  w.key("sync_id");
  w.valueUuid(syncId);
  w.kvSeq("base_revision", baseRevision);
  writeBook(w, book);
  w.objectEnd();
}

bool writeSession(JsonWriter& w, const BookId& book, const SessionSummary& s) {
  // started_at and ended_at are required timestamps: a session with an uncertain
  // clock has nothing honest to report.
  if (s.started.precision < TimePrecision::Anchored || s.ended.precision < TimePrecision::Anchored) return false;
  const int64_t duration = s.ended.epochSec - s.started.epochSec;
  if (duration < kMinSessionSec || duration > kMaxSessionSec) return false;

  char started[21], ended[21];
  formatRfc3339(s.started.epochSec, started);
  formatRfc3339(s.ended.epochSec, ended);
  char aggregate[8 + 32 + 1 + 20 + 1];
  memcpy(aggregate, "session:", 8);
  memcpy(aggregate + 8, book.hex, 32);
  aggregate[40] = ':';
  memcpy(aggregate + 41, started, 21);

  EventTime time;
  time.wall = s.ended;
  writeHead(w, "session.ended", "reading_session");
  w.value(aggregate);
  writeTail(w, book, time);
  w.objectBegin();
  w.kv("started_at", started);
  w.kv("ended_at", ended);
  w.kvInt("duration_seconds", duration);
  w.kvInt("pages_read", s.pagesRead < 0 ? 0 : s.pagesRead);
  if (s.startPage >= 0) w.kvInt("start_page", s.startPage);
  if (s.endPage >= 0) w.kvInt("end_page", s.endPage);
  w.objectEnd();
  return true;
}

void writePageStats(JsonWriter& w, const BookRef& book, TimePrecision precision, const PageStatRow* rows, uint8_t count,
                    uint8_t chunkIndex) {
  const PageStatRow& last = rows[count - 1];
  char first[21], newest[21], index[21];
  formatU64(rows[0].startEpoch, first);
  formatU64(last.startEpoch, newest);
  formatU64(chunkIndex, index);
  // main.lua:_enqueueStatsV2. A chunk is identified by what it holds, not by when it was sent.
  char aggregate[11 + 32 + 1 + 10 + 1 + 10 + 1 + 3 + 1];
  JsonWriter id(aggregate, sizeof(aggregate) - 1);
  id.raw("page-stats:");
  id.raw(book.id.hex);
  id.raw(":");
  id.raw(first);
  id.raw(":");
  id.raw(newest);
  id.raw(":");
  id.raw(index);
  aggregate[id.size()] = '\0';

  EventTime time;
  time.wall = WallTime{last.startEpoch, precision};
  writeHead(w, "page_stat.recorded", "reading_statistics");
  w.value(aggregate);
  writeTail(w, book.id, time);
  w.objectBegin();
  w.key("rows");
  w.arrayBegin();
  for (uint8_t i = 0; i < count; ++i) {
    w.objectBegin();
    w.kv("book_md5", book.id.hex);
    w.kvInt("page", rows[i].page);
    w.kvInt("start_time", rows[i].startEpoch);
    w.kvInt("duration", rows[i].seconds);
    if (book.totalPages >= 1) w.kvInt("total_pages", book.totalPages);
    w.objectEnd();
  }
  w.arrayEnd();
  w.key("books");
  w.arrayBegin();
  w.objectBegin();
  w.kv("md5", book.id.hex);
  // Over its limit a field would get the whole chunk refused: boundedMetadata leaves it out.
  if (book.title && *book.title && strlen(book.title) <= kMaxStatsText) w.kv("title", book.title);
  if (book.author && *book.author && strlen(book.author) <= kMaxStatsText) w.kv("authors", book.author);
  if (book.totalPages >= 1) w.kvInt("pages", book.totalPages);
  w.objectEnd();
  w.arrayEnd();
  w.objectEnd();
}

uint64_t clipLedgerKey(const BookId& book, const ClipKey& clip) {
  uint64_t h = fnv1a64(book.md5, 16);
  uint8_t raw[8 + 8];
  ByteWriter w(raw, sizeof(raw));
  w.u32(clip.spine);
  w.u32(clip.textOffset);
  w.u64(clip.textHash);
  return fnv1a64(raw, sizeof(raw), h);
}

void deriveSyncId(const BookId& book, const ClipKey& clip, uint8_t out[16]) {
  // Derived, so a crash between "event queued" and "ledger saved" produces the same
  // id on the retry and the server sees one annotation, not two.
  uint8_t seed[16 + 16];
  ByteWriter w(seed, sizeof(seed));
  w.bytes(book.md5, 16);
  w.u32(clip.spine);
  w.u32(clip.textOffset);
  w.u64(clip.textHash);
  uuidFromSeed(seed, sizeof(seed), out);
}

}  // namespace events

namespace {

void parseOrigin(JsonScanner& scan, RemoteEvent& out) {
  if (!scan.enterObject()) {
    scan.skipValue();
    return;
  }
  char key[16];
  while (scan.nextMember(key, sizeof(key))) {
    if (strcmp(key, "id") == 0) {
      scan.readString(out.originId, sizeof(out.originId));
    } else if (strcmp(key, "name") == 0) {
      scan.readString(out.originName, sizeof(out.originName));
    } else {
      scan.skipValue();
    }
  }
}

void parseBookIdentifier(JsonScanner& scan, RemoteEvent& out) {
  if (!scan.enterObject()) {
    scan.skipValue();
    return;
  }
  char key[16];
  bool truncated = false;
  while (scan.nextMember(key, sizeof(key))) {
    if (strcmp(key, "kind") == 0) {
      scan.readString(out.bookKind, sizeof(out.bookKind), &truncated);
      if (truncated) out.bookKind[0] = '\0';
    } else if (strcmp(key, "value") == 0) {
      scan.readString(out.bookValue, sizeof(out.bookValue), &truncated);
      if (truncated) out.bookValue[0] = '\0';
    } else {
      scan.skipValue();
    }
  }
}

void parsePayload(JsonScanner& scan, RemoteEvent& out) {
  if (!scan.enterObject()) {
    scan.skipValue();
    return;
  }
  char key[20];
  int64_t number = 0;
  while (scan.nextMember(key, sizeof(key))) {
    if (strcmp(key, "percentage") == 0) {
      out.hasPct = scan.readPercentMicro(out.pctMicro);
    } else if (strcmp(key, "current_page") == 0) {
      if (scan.readInt(number) && number >= 0 && number <= 0x7FFFFFFF) out.page = static_cast<int32_t>(number);
    } else if (strcmp(key, "total_pages") == 0) {
      if (scan.readInt(number) && number >= 1 && number <= 0x7FFFFFFF) out.totalPages = static_cast<int32_t>(number);
    } else if (strcmp(key, "xpointer") == 0) {
      bool truncated = false;
      out.xpointerUsable =
          scan.readString(out.xpointer, sizeof(out.xpointer), &truncated) && !truncated && out.xpointer[0];
      if (!out.xpointerUsable) out.xpointer[0] = '\0';
    } else if (strcmp(key, "explicit_jump") == 0) {
      scan.readBool(out.explicitJump);
    } else if (strcmp(key, "sync_id") == 0) {
      bool truncated = false;
      scan.readString(out.syncId, sizeof(out.syncId), &truncated);
      if (truncated) out.syncId[0] = '\0';
    } else {
      scan.skipValue();
    }
  }
}

void parseDirectiveMetadata(JsonScanner& scan, RemoteEvent& out) {
  if (!scan.enterObject()) {
    scan.skipValue();
    return;
  }
  char key[20];
  while (scan.nextMember(key, sizeof(key))) {
    if (strcmp(key, "reading_head") == 0) {
      scan.readBool(out.readingHead);
    } else if (strcmp(key, "annotation_revision") == 0) {
      if (!scan.readSequence(out.annotationRevision)) out.annotationRevision = 0;
    } else if (strcmp(key, "suggestion") == 0) {
      if (scan.enterObject()) {
        char inner[12];
        while (scan.nextMember(inner, sizeof(inner))) {
          if (strcmp(inner, "id") == 0) {
            bool truncated = false;
            scan.readString(out.suggestionId, sizeof(out.suggestionId), &truncated);
            if (truncated || !isCanonicalUuid(out.suggestionId)) out.suggestionId[0] = '\0';
          } else {
            scan.skipValue();
          }
        }
      } else {
        scan.skipValue();
      }
    } else {
      scan.skipValue();
    }
  }
}

}  // namespace

bool RemoteEvent::isProgress() const { return strcmp(eventType, "progress.changed") == 0; }

bool RemoteEvent::isAnnotation() const {
  return strncmp(eventType, "annotation.", 11) == 0 || strncmp(eventType, "bookmark.", 9) == 0;
}

bool RemoteEvent::isAnnotationDelete() const {
  return strcmp(eventType, "annotation.deleted") == 0 || strcmp(eventType, "bookmark.deleted") == 0;
}

bool parseRemoteEvent(JsonScanner& scan, RemoteEvent& out) {
  out = RemoteEvent{};
  if (!scan.enterObject()) return scan.skipValue();
  char key[24];
  while (scan.nextMember(key, sizeof(key))) {
    if (strcmp(key, "event_id") == 0) {
      bool truncated = false;
      out.hasEventId = scan.readString(out.eventId, sizeof(out.eventId), &truncated) && !truncated && out.eventId[0];
      if (!out.hasEventId) out.eventId[0] = '\0';
    } else if (strcmp(key, "server_sequence") == 0) {
      out.hasSeq = scan.readSequence(out.serverSeq);
    } else if (strcmp(key, "event_type") == 0) {
      scan.readString(out.eventType, sizeof(out.eventType));
    } else if (strcmp(key, "book_identifier") == 0) {
      parseBookIdentifier(scan, out);
    } else if (strcmp(key, "origin_device") == 0) {
      parseOrigin(scan, out);
    } else if (strcmp(key, "occurred_at") == 0) {
      scan.readString(out.occurredAt, sizeof(out.occurredAt));
    } else if (strcmp(key, "time_precision") == 0) {
      char text[16];
      if (scan.readString(text, sizeof(text))) parsePrecision(text, out.precision);
    } else if (strcmp(key, "payload") == 0) {
      parsePayload(scan, out);
    } else if (strcmp(key, "directive") == 0) {
      scan.readString(out.directive, sizeof(out.directive));
    } else if (strcmp(key, "directive_metadata") == 0) {
      parseDirectiveMetadata(scan, out);
    } else {
      scan.skipValue();
    }
  }
  return !scan.failed();
}

}  // namespace borges
