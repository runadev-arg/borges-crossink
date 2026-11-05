// A second client that sends exactly what borges.koplugin sends. Each method names the
// Lua it mirrors. No Lua interpreter is available on this machine, so this is a port and
// not the plugin itself: envelope and payload are copied field by field from
//   queue.lua        enqueueEvent, copyWireEvent, prepareBatch, commitExchange
//   syncv2.lua       pull, sync, recoverSequences flow
//   pairing.lua      start, claim
//   main.lua         _progressPayload, _enqueueProgressV2, _enqueueSessionV2, client(), _handleReadingHead
//   annotationadapter.lua  normalize, diff

import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";

// pairing.lua REQUESTED_SCOPES
const REQUESTED_SCOPES = ["device:self", "sync:v2", "sync:legacy", "library:read", "plugin:update", "kosync"];

const utcSeconds = (epoch) => new Date(epoch * 1000).toISOString().replace(/\.\d{3}Z$/, "Z");

export class ApiFailure extends Error {
  constructor(status, body) {
    super(`HTTP ${status}: ${JSON.stringify(body)?.slice(0, 300)}`);
    this.status = status;
    this.code = body?.error?.code ?? body?.code ?? null;
    this.details = body?.error?.details ?? null;
    this.body = body;
  }
}

export class KoreaderSim {
  constructor({ baseUrl, kobo, name = "Kobo Libra", model = "KoboLibraH2O", platform = "koreader", externalId = null }) {
    this.baseUrl = baseUrl;
    this.name = name;
    this.model = model;
    this.platform = platform;
    this.installId = externalId ?? crypto.randomUUID();
    this.pluginVersion = fs.readFileSync(path.join(kobo, "borges.koplugin", "plugin_version.lua"), "utf8").match(/"([^"]+)"/)[1];
    this.token = null;
    this.deviceId = null;
    this.nextSequence = 1;
    this.cursor = "0";
    this.outbox = [];
    this.inbox = [];
    this.applied = [];
    this.annotationState = new Map();
    this.choices = new Set();
  }

  async post(url, body, auth = true) {
    const headers = { "Content-Type": "application/json", Accept: "application/json" };
    if (auth && this.token) headers.Authorization = `Bearer ${this.token}`;
    const res = await fetch(`${this.baseUrl}${url}`, { method: "POST", headers, body: JSON.stringify(body) });
    const text = await res.text();
    const parsed = text ? JSON.parse(text) : null;
    if (!res.ok) throw new ApiFailure(res.status, parsed);
    return parsed;
  }

  async get(url) {
    const res = await fetch(`${this.baseUrl}${url}`, { headers: { Authorization: `Bearer ${this.token}`, Accept: "application/json" } });
    const parsed = await res.json();
    if (!res.ok) throw new ApiFailure(res.status, parsed);
    return parsed;
  }

  // pairing.lua Pairing:start
  async pairingRequest() {
    this.nonce = crypto.randomUUID();
    const response = await this.post(
      "/api/devices/pairing/request",
      {
        device_nonce: this.nonce,
        device_name: this.name,
        platform: this.platform,
        external_id: this.installId,
        firmware_version: this.model,
        client_version: this.pluginVersion,
        protocol_version: 2,
        requested_scopes: REQUESTED_SCOPES,
        capabilities: {
          progress: true,
          sessions: true,
          page_stats: true,
          annotations: true,
          library_download: true,
          updater: true,
          durable_outbox: true,
        },
      },
      false
    );
    this.requestId = response.request_id;
    return response;
  }

  async pairingStatus() {
    return this.post("/api/devices/pairing/status", { request_id: this.requestId, device_nonce: this.nonce }, false);
  }

  async pairingClaim() {
    const response = await this.post("/api/devices/pairing/claim", { request_id: this.requestId, device_nonce: this.nonce }, false);
    this.token = response.credential.token;
    this.deviceId = response.device.id;
    return response;
  }

  // main.lua client()
  client() {
    return {
      client_version: this.pluginVersion,
      firmware_version: this.model,
      protocol_version: 2,
      capabilities: {
        platform: "koreader",
        durable_outbox: true,
        progress: true,
        sessions: true,
        page_stats: true,
        annotations: true,
      },
      queue_depth: this.outbox.length,
    };
  }

  // queue.lua Queue:enqueueEvent
  enqueue(event, { coalesceUnattempted = false } = {}) {
    if (coalesceUnattempted) {
      for (let i = this.outbox.length - 1; i >= 0; i -= 1) {
        const existing = this.outbox[i];
        if (
          existing.event_type === event.event_type &&
          existing.aggregate_type === event.aggregate_type &&
          existing.aggregate_id === event.aggregate_id &&
          existing._attempts === 0
        ) {
          existing.occurred_at = event.occurred_at;
          existing.time_precision = event.time_precision ?? "exact";
          existing.payload = event.payload;
          return existing;
        }
      }
    }
    const stored = {
      client_event_id: crypto.randomUUID(),
      client_sequence: String(this.nextSequence),
      event_version: 1,
      event_type: event.event_type,
      aggregate_type: event.aggregate_type,
      aggregate_id: event.aggregate_id,
      document_id: event.document_id ?? null,
      book_identifier: event.book_identifier,
      occurred_at: event.occurred_at ?? utcSeconds(Math.floor(Date.now() / 1000)),
      time_precision: event.time_precision ?? "exact",
      payload: event.payload,
      _attempts: 0,
    };
    this.nextSequence += 1;
    this.outbox.push(stored);
    return stored;
  }

  static bookIdentifier(hash) {
    return { kind: "koreader_partial_md5", value: hash.toLowerCase() };
  }

  // main.lua _progressPayload + _enqueueProgressV2
  enqueueProgress({ book, page, totalPages, xpointer, isJump = false, title, author, epoch = Math.floor(Date.now() / 1000), precision = "exact" }) {
    const percentage = totalPages > 0 ? Math.floor((page / totalPages) * 1000000) / 10000 : 0;
    return this.enqueue(
      {
        event_type: "progress.changed",
        aggregate_type: "reading_progress",
        aggregate_id: `koreader_partial_md5:${book}`,
        book_identifier: KoreaderSim.bookIdentifier(book),
        occurred_at: utcSeconds(epoch),
        time_precision: precision,
        payload: { percentage, current_page: page, total_pages: totalPages, xpointer, explicit_jump: isJump === true, title, author },
      },
      { coalesceUnattempted: true }
    );
  }

  // main.lua _enqueueSessionV2
  enqueueSession({ book, startedAt, endedAt, duration, pagesRead = 1, startPage = 1, endPage = 2 }) {
    return this.enqueue({
      event_type: "session.ended",
      aggregate_type: "reading_session",
      aggregate_id: `session:${book}:${startedAt}`,
      book_identifier: KoreaderSim.bookIdentifier(book),
      occurred_at: endedAt,
      time_precision: "exact",
      payload: {
        started_at: startedAt,
        ended_at: endedAt,
        duration_seconds: duration,
        pages_read: pagesRead,
        start_page: startPage,
        end_page: endPage,
        timezone: "UTC",
        explicit_jump: false,
      },
    });
  }

  // annotationadapter.lua uuidFromSeed + normalize, for rolling (EPUB) documents.
  static uuidFromSeed(seed) {
    const hex = crypto.createHash("md5").update(seed, "utf8").digest("hex");
    return [hex.slice(0, 8), hex.slice(8, 12), `3${hex.slice(13, 16)}`, `8${hex.slice(17, 20)}`, hex.slice(20, 32)].join("-");
  }

  static annotationSyncId(book, { kind, xpointer, pos1 = "" }) {
    return KoreaderSim.uuidFromSeed(`${book.toLowerCase()}|${kind}|rolling:${xpointer}:${pos1}`);
  }

  // annotationadapter.lua AnnotationAdapter:diff, one item. KOReader sends null fields as
  // absent because rapidjson drops nil; undefined does the same here.
  enqueueAnnotation({ book, bookInfo, kind = "highlight", text, note, chapter, page, xpointer, pos1, color, drawer, timeMs, epoch = Math.floor(Date.now() / 1000), syncId, baseRevision }) {
    const sync = syncId ?? KoreaderSim.annotationSyncId(book, { kind, xpointer, pos1: pos1 ?? "" });
    const previous = this.annotationState.get(sync);
    const revision = baseRevision ?? previous?.revision ?? "0";
    const payload = {
      sync_id: sync,
      kind,
      text,
      note,
      chapter,
      page,
      total_pages: bookInfo?.total_pages,
      time: timeMs,
      color,
      drawer: kind === "bookmark" ? "bookmark" : drawer ?? "lighten",
      xpointer,
      position: pos1 ? { pos0: xpointer, pos1 } : undefined,
      base_revision: revision,
      book: bookInfo,
    };
    const eventType = kind === "bookmark" ? "bookmark.upserted" : previous ? "annotation.updated" : "annotation.created";
    this.annotationState.set(sync, { kind, revision });
    return this.enqueue({
      event_type: eventType,
      aggregate_type: "annotation",
      aggregate_id: sync,
      book_identifier: KoreaderSim.bookIdentifier(book),
      occurred_at: utcSeconds(epoch),
      time_precision: "exact",
      payload,
    });
  }

  enqueueAnnotationDelete({ book, bookInfo, syncId, kind = "highlight", epoch = Math.floor(Date.now() / 1000) }) {
    const previous = this.annotationState.get(syncId);
    return this.enqueue({
      event_type: kind === "bookmark" ? "bookmark.deleted" : "annotation.deleted",
      aggregate_type: "annotation",
      aggregate_id: syncId,
      book_identifier: KoreaderSim.bookIdentifier(book),
      occurred_at: utcSeconds(epoch),
      time_precision: "exact",
      payload: { sync_id: syncId, base_revision: previous?.revision ?? "0", book: bookInfo },
    });
  }

  static wire(event) {
    const { _attempts, ...wire } = event;
    return wire;
  }

  // queue.lua commitExchange. Everything goes to the inbox: a test reads it from there.
  commit(response) {
    const pull = response.pull ?? response;
    const removals = new Set();
    const acks = response.acknowledgements ?? [];
    for (const ack of acks) {
      const event = this.outbox.find((e) => e.client_event_id === ack.client_event_id);
      if (event) {
        if (String(ack.client_sequence) !== String(event.client_sequence)) throw new Error("ack sequence mismatch");
        removals.add(event.client_event_id);
        const metadata = ack.metadata ?? {};
        if (event.aggregate_type === "annotation" && metadata.annotation_revision != null) {
          const state = this.annotationState.get(event.aggregate_id);
          if (state) state.revision = String(metadata.annotation_revision);
        }
      }
    }
    this.outbox = this.outbox.filter((e) => !removals.has(e.client_event_id));
    for (const event of pull.events) this.inbox.push(event);
    this.cursor = String(pull.cursor);
    return { acknowledgements: acks, events: pull.events, cursor: this.cursor, hasMore: pull.has_more === true, pending: this.outbox.length };
  }

  // syncv2.lua SyncV2:pull
  async pull({ focusBook, focusOnly } = {}) {
    const response = await this.post("/api/sync/v2/pull", {
      protocol_version: 2,
      cursor: this.cursor,
      limit: 20,
      focus_book: focusBook ? KoreaderSim.bookIdentifier(focusBook) : undefined,
      focus_only: focusOnly === true ? true : undefined,
      client: this.client(),
    });
    response.acknowledgements = [];
    const committed = this.commit(response);
    committed.focusEvent = response.focus_event;
    committed.headChecked = response.reading_head_checked;
    return committed;
  }

  // syncv2.lua SyncV2:sync with prepareBatch's defaults: everything fits within 100 events here.
  async sync({ maxEvents = 100 } = {}) {
    const events = this.outbox.slice(0, maxEvents);
    for (const e of events) e._attempts += 1;
    if (events.length === 0) {
      return this.commit(await this.post("/api/sync/v2/pull", { protocol_version: 2, cursor: this.cursor, limit: 20, client: this.client() }));
    }
    const response = await this.post("/api/sync/v2/exchange", {
      protocol_version: 2,
      cursor: this.cursor,
      events: events.map(KoreaderSim.wire),
      client: this.client(),
    });
    if (response.acknowledgements === undefined) response.acknowledgements = [];
    return this.commit(response);
  }

  // syncv2.lua SyncV2:syncAll
  async syncAll({ maxPages = 50, maxEvents = 100 } = {}) {
    const acks = [];
    const events = [];
    for (let page = 0; page < maxPages; page += 1) {
      const result = await this.sync({ maxEvents });
      acks.push(...result.acknowledgements);
      events.push(...result.events);
      if (!result.hasMore && result.pending === 0) break;
    }
    return { acks, events, pending: this.outbox.length, cursor: this.cursor };
  }

  // main.lua _handleReadingHead + resumeflow.lua hasLocalChoice, on a focus_only answer.
  decideHead(result, book) {
    if (result.headChecked !== true) return { valid: false };
    const event = result.focusEvent;
    if (event == null) return { valid: true, ask: false, event: null };
    const source = event.origin_device;
    const valid =
      event.book_identifier?.value === book &&
      event.book_identifier?.kind === "koreader_partial_md5" &&
      source?.id &&
      event.event_id &&
      event.directive_metadata?.reading_head === true;
    if (!valid) return { valid: false, event };
    const own = source.id === this.deviceId;
    const chosen = this.choices.has(`event:${event.event_id}`);
    return { valid: true, ask: !own && !chosen, own, event };
  }

  remember(event) {
    this.choices.add(`event:${event.event_id}`);
  }
}
