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
  w.valuePercent(position.hasPct ? (position.pctMicro > 100000000u ? 100000000u : position.pctMicro) : 0);
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

bool writeAnnotation(JsonWriter& w, const BookRef& book, const EventTime& time, const ClipKey& key,
                     const ClipDetail& d, const uint8_t syncId[16]) {
  if (!hasNonSpace(d.text)) return false;
  const bool hasXp = d.xpointer && *d.xpointer;
  if (!hasXp && d.page < 0) return false;  // annotation_locator_required.
  writeHead(w, key.bookmark ? "bookmark.upserted" : "annotation.created", "annotation");
  w.valueUuid(syncId);
  writeTail(w, book.id, time);
  w.objectBegin();
  w.key("sync_id");
  w.valueUuid(syncId);
  w.kv("kind", key.bookmark ? "bookmark" : "highlight");
  w.kv("text", d.text);
  if (d.note && *d.note) w.kv("note", d.note);
  if (d.chapter && *d.chapter) w.kv("chapter", d.chapter);
  if (d.page >= 0) w.kvInt("page", d.page);
  if (book.totalPages >= 1) w.kvInt("total_pages", book.totalPages);
  if (d.timeMs > 0) w.kvInt("time", d.timeMs);
  w.kv("color", d.color && *d.color ? d.color : "gray");
  w.kv("drawer", key.bookmark ? "bookmark" : "lighten");
  if (hasXp) w.kv("xpointer", d.xpointer);
  if (d.hasPct) {
    w.key("percentage");
    w.valuePercent(d.pctMicro > 100000000u ? 100000000u : d.pctMicro);
  }
  writeBook(w, book);
  w.objectEnd();
  return true;
}

void writeAnnotationDeleted(JsonWriter& w, const BookRef& book, const EventTime& time, bool bookmark,
                            const uint8_t syncId[16]) {
  writeHead(w, bookmark ? "bookmark.deleted" : "annotation.deleted", "annotation");
  w.valueUuid(syncId);
  writeTail(w, book.id, time);
  w.objectBegin();
  w.key("sync_id");
  w.valueUuid(syncId);
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
    } else if (strcmp(key, "page") == 0) {
      if (scan.readInt(number) && number >= 0 && number <= 0x7FFFFFFF) out.annotationPage = static_cast<int32_t>(number);
    } else if (strcmp(key, "xpointer") == 0) {
      bool truncated = false;
      out.xpointerUsable = scan.readString(out.xpointer, sizeof(out.xpointer), &truncated) && !truncated && out.xpointer[0];
      if (!out.xpointerUsable) out.xpointer[0] = '\0';
    } else if (strcmp(key, "explicit_jump") == 0) {
      scan.readBool(out.explicitJump);
    } else if (strcmp(key, "sync_id") == 0) {
      bool truncated = false;
      scan.readString(out.syncId, sizeof(out.syncId), &truncated);
      if (truncated) out.syncId[0] = '\0';
    } else if (strcmp(key, "text") == 0) {
      scan.readString(out.text, sizeof(out.text), &out.textTruncated);
    } else if (strcmp(key, "note") == 0) {
      scan.readString(out.note, sizeof(out.note));
    } else if (strcmp(key, "chapter") == 0) {
      scan.readString(out.chapter, sizeof(out.chapter));
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
