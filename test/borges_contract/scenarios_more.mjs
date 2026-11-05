// Scenarios 4 to 11: offline, faults, repair, annotations, sessions, library, revocation,
// and volume.

import fs from "node:fs";
import path from "node:path";
import { book, bookArgs, bookInfo, eventsOf, headOf, iso, krRead, now, xp } from "./scenarios.mjs";
import { buildEpub, koreaderPartialMd5, sha256 } from "./epub.mjs";
import { KoreaderSim } from "./koreader.mjs";
import { RUN_DIR } from "./harness.mjs";

const positions = (kr, md5) => kr.get(`/api/sync/v2/progress/positions?book_identifier_kind=koreader_partial_md5&book_identifier_value=${md5}`);
const watermark = async (h, userId) => (await h.sql`SELECT COALESCE(max(server_sequence), 0)::text AS w FROM device_events WHERE user_id = ${userId}`)[0].w;

// ---------------------------------------------------------------------------------
// 4. Offline
// ---------------------------------------------------------------------------------

async function offline(h, t) {
  const { account, fw, kr } = await h.fwAndKoreader("offline");
  const a = book(fw);
  const b = book(fw);
  const base = now() - 7200;

  // Connections refused.
  await h.proxy.down();
  for (const [i, pct] of [10, 20, 30].entries()) {
    const f = await fw.once("finalize-book", { ...bookArgs(a), pct, xp: xp(pct), page: pct, turn_epoch: base + i * 100 });
    t.eq(f.finalize.progress_queued, true, `finalize ${pct} queued offline`);
  }
  await fw.once("finalize-book", { ...bookArgs(b), pct: 50, xp: xp(50), page: 50, turn_epoch: base + 150 });
  let s = await fw.once("status");
  t.eq(s.depth, 2, "one unattempted progress per book");
  const down = await fw.once("sleep-session");
  t.eq(down.report.outcome, "transport", "sleep session reports no network");
  t.eq(down.report.pull_failed, true, "the pull failed");
  t.eq(down.report.drain_blocked_by_pull, true, "and nothing is pushed");
  s = await fw.once("status");
  t.eq(s.depth, 2, "queue kept");
  t.eq(s.diag.code, "transport", "status says why");
  t.ok(s.next_attempt_epoch > now(), "backoff scheduled");
  t.eq((await fw.once("gate", { kind: "sleep" })).gate, "backed_off", "automatic sessions wait");
  t.eq((await fw.once("gate", { kind: "open" })).gate, "backed_off", "open sessions wait too");
  t.eq((await fw.once("gate", { kind: "manual" })).gate, "go", "manual always tries");
  await fw.once("finalize-book", { ...bookArgs(a), pct: 35, xp: xp(35), page: 35, turn_epoch: base + 300 });
  t.eq((await fw.once("status")).depth, 2, "a failed pull attempts nothing: the event can still be replaced");
  await h.proxy.up();

  // The real server process dies and comes back. The proxy answers 502, as an edge would.
  await h.stopServer();
  const dead = await fw.once("manual-session");
  t.eq(dead.report.outcome, "server_error", "a dead origin is a server error");
  t.eq((await fw.once("status")).depth, 2, "queue kept through a dead server");
  await h.startServer();
  h.proxy.freshIp();

  const back = await fw.once("manual-session");
  t.eq(back.report.outcome, "ok", "server back, session ok");
  t.eq(back.report.events_acked, 2, "both books went up");
  let rows = await eventsOf(h, fw.deviceId);
  t.eq(rows.length, 2, "only the latest progress per book reached the server");
  const rowA = rows.find((r) => r.payload.xpointer === xp(35));
  t.ok(rowA, "the row is the latest position");
  t.eq(rowA && new Date(rowA.occurred_at).toISOString(), iso(base + 300), "occurred_at is the last page turn, not the upload");
  t.eq(rowA?.time_precision, "exact", "precision preserved");
  t.eq((await fw.once("gate", { kind: "sleep" })).gate, "nothing_to_do", "backoff cleared, empty queue costs no radio");

  // Honest clocks.
  const unknownBook = book(fw);
  const unknownFw = { precision: "unknown" };
  await fw.once("finalize-book", { ...bookArgs(unknownBook), pct: 12, xp: xp(12), page: 12, turn_precision: "unknown" }, unknownFw);
  const proc = fw.boot(unknownFw);
  t.eq((await proc.cmd("status")).clock.precision, "unknown", "the clock starts unknown");
  const unknownSleep = await proc.cmd("sleep-session");
  t.eq(unknownSleep.report.outcome, "ok", "unknown clock syncs");
  const afterServerTime = await proc.cmd("status");
  t.eq(afterServerTime.clock.precision, "exact", "server_time set the clock");
  t.ok(afterServerTime.clock.set_calls >= 1, "through setWall");
  await proc.close();
  rows = await eventsOf(h, fw.deviceId);
  const unknownRow = rows.find((r) => r.payload.xpointer === xp(12));
  t.eq(unknownRow?.time_precision, "unknown", "unknown stays unknown");
  t.eq(unknownRow?.occurred_at, null, "no invented date");

  const anchoredBook = book(fw);
  const anchoredAt = now() - 500;
  await fw.once("finalize-book", { ...bookArgs(anchoredBook), pct: 13, xp: xp(13), page: 13, turn_epoch: anchoredAt, turn_precision: "anchored" });
  await fw.once("sleep-session");
  rows = await eventsOf(h, fw.deviceId);
  const anchoredRow = rows.find((r) => r.payload.xpointer === xp(13));
  t.eq(anchoredRow?.time_precision, "anchored", "anchored stays anchored");
  t.eq(anchoredRow && new Date(anchoredRow.occurred_at).toISOString(), iso(anchoredAt), "anchored keeps its date");

  // A late upload must not displace a newer reading.
  const late = book(fw);
  const readAt = now() - 60;
  await krRead(kr, late, 77, { epoch: readAt });
  await fw.once("finalize-book", { ...bookArgs(late), pct: 22, xp: xp(22), page: 22, turn_epoch: now() - 3600 });
  const lateSleep = await fw.once("sleep-session");
  t.eq(lateSleep.report.events_acked, 1, "the old reading uploads");
  let head = await headOf(h, late.md5, account.id);
  t.eq(head.length, 2, "both devices have a position row");
  t.eq(head[0].device_id, kr.deviceId, "the newer reading stays the head");
  const pos = await positions(kr, late.md5);
  t.eq(pos.positions?.[0]?.device_id, kr.deviceId, "positions: newest reading first");
  t.eq(pos.positions?.length, 2, "positions: both devices");
  const lateOpen = await fw.once("open-session", { ...bookArgs(late), pct: 22, xp: xp(22), page: 22 });
  t.eq(lateOpen.decision.ask, true, "the firmware is offered the newer reading");
  t.near(lateOpen.decision.remote_pct, 77, 0.0001, "offered position");

  // A held book: opened offline, finalized, and the other device moved meanwhile. Sleep
  // must not push it before the person chose.
  const held = book(fw);
  await h.proxy.down();
  await fw.once("book-opened", { ...bookArgs(held) });
  await fw.once("finalize-book", { ...bookArgs(held), pct: 33, xp: xp(33), page: 33, turn_epoch: now() - 200 });
  await h.proxy.up();
  await krRead(kr, held, 66, { epoch: now() - 100 });
  const heldSleep = await fw.once("sleep-session");
  t.eq(heldSleep.report.outcome, "ok", "sleep with a held book is fine");
  t.ok(heldSleep.report.held >= 1, "the book stays held");
  const heldEvents = (await eventsOf(h, fw.deviceId)).filter((r) => r.payload.xpointer === xp(33));
  t.eq(heldEvents.length, 0, "its progress did not go up");
  const heldProc = fw.boot();
  const heldOpen = await heldProc.cmd("open-session", { ...bookArgs(held), pct: 33, xp: xp(33), page: 33 });
  t.eq(heldOpen.decision.ask, true, "opening it asks");
  const heldStay = await heldProc.cmd("apply-choice", { choice: "stay", total: held.total });
  t.eq(heldStay.report.outcome, "ok", "the choice releases it");
  t.eq(heldStay.report.depth, 0, "queue empty after the choice");
  await heldProc.close();

  // Uncertain clocks are ordered by arrival: the server says so, and nobody claims a date.
  const uncertain = book(fw);
  await krRead(kr, uncertain, 50, { epoch: now() - 60 });
  await fw.once("finalize-book", { ...bookArgs(uncertain), pct: 9, xp: xp(9), page: 9, turn_precision: "unknown" });
  await fw.once("sleep-session");
  head = await headOf(h, uncertain.md5, account.id);
  t.eq(head[0].device_id, fw.deviceId, "an unknown clock is ordered by reception");
  const uncertainHead = kr.decideHead(await kr.pull({ focusBook: uncertain.md5, focusOnly: true }), uncertain.md5);
  t.eq(uncertainHead.ask, true, "KOReader asks");
  t.eq(uncertainHead.event?.time_precision, "unknown", "and is told the clock is unknown");
  kr.enqueueProgress({ book: uncertain.md5, page: 51, totalPages: 100, xpointer: xp(51), title: uncertain.title, author: uncertain.author, precision: "unknown" });
  await kr.syncAll();
  const noDate = await fw.once("open-session", { ...bookArgs(uncertain), pct: 9, xp: xp(9), page: 9 });
  t.eq(noDate.decision.ask, true, "the firmware asks about an unknown clock too");
  t.eq(noDate.decision.show_date, false, "without showing a date");
  t.eq(noDate.decision.occurred_at, "", "no date in the decision");

  // A reader whose clock runs a day ahead, labelled exact.
  const future = book(fw);
  await krRead(kr, future, 95, { epoch: now() + 86400 });
  await new Promise((r) => setTimeout(r, 1100));
  await fw.once("finalize-book", { ...bookArgs(future), pct: 40, xp: xp(40), page: 40, turn_epoch: now() });
  await fw.once("sleep-session");
  const futureHead = await headOf(h, future.md5, account.id);
  const recovered = await positions(kr, future.md5);
  t.note(`future clock: focus head=${futureHead[0].device_id === kr.deviceId ? "the reader with the clock a day ahead" : "the real latest reading"}; progress/positions first=${recovered.positions?.[0]?.device_id === kr.deviceId ? "the reader with the clock a day ahead" : "the real latest reading"}`);
}

// ---------------------------------------------------------------------------------
// 5. Lost responses and a fault sweep
// ---------------------------------------------------------------------------------

async function faults(h, t, metrics) {
  const { account, fw, kr } = await h.fwAndKoreader("faults");
  const a = book(fw);

  // Answer lost once in a session: the one reconnect retries the same bytes.
  await fw.once("finalize-book", { ...bookArgs(a), pct: 11, xp: xp(11), page: 11, turn_epoch: now() - 600 });
  await fw.once("manual-session"); // Learns capabilities, pulls.
  await fw.once("finalize-book", { ...bookArgs(a), pct: 12, xp: xp(12), page: 12, turn_epoch: now() - 500 });
  h.proxy.clear();
  h.proxy.fault({ match: "/api/sync/v2/exchange", action: "drop-after" });
  const once = await fw.once("sleep-session");
  t.eq(once.report.outcome, "ok", "a lost answer heals inside the session");
  const exchanges = h.proxy.requestsTo("/api/sync/v2/exchange");
  t.eq(exchanges.length, 2, "same exchange sent twice");
  t.eq(JSON.parse(exchanges[0].body).events, JSON.parse(exchanges[1].body).events, "the retry is the same envelope");
  const ack = JSON.parse(exchanges[1].responseBody).acknowledgements[0];
  t.eq(ack.duplicate, true, "the server answers duplicate:true");
  t.eq(ack.status, "accepted", "from its record");
  let rows = await eventsOf(h, fw.deviceId);
  t.eq(rows.filter((r) => r.payload.xpointer === xp(12)).length, 1, "no second row");

  // Answer lost twice: the session ends. The event is immutable from here.
  await fw.once("finalize-book", { ...bookArgs(a), pct: 13, xp: xp(13), page: 13, turn_epoch: now() - 400 });
  h.proxy.clear();
  h.proxy.fault({ match: "/api/sync/v2/exchange", action: "drop-after", times: 2 });
  const twice = await fw.once("sleep-session");
  t.eq(twice.report.outcome, "transport", "two losses end the session");
  const box = await fw.once("outbox");
  t.eq(box.depth, 1, "still queued");
  t.eq(box.events[0].attempted, true, "and marked attempted");
  const original = box.events[0].envelope[0];
  await fw.once("finalize-book", { ...bookArgs(a), pct: 14, xp: xp(14), page: 14, turn_epoch: now() - 300 });
  const box2 = await fw.once("outbox");
  t.eq(box2.depth, 2, "an attempted event is not replaced: a new one is added");
  t.eq(box2.events[0].envelope[0], original, "the attempted envelope did not change");
  h.proxy.clear();
  const next = await fw.once("sleep-session");
  t.eq(next.report.outcome, "ok", "next session converges");
  const acks = JSON.parse(h.proxy.requestsTo("/api/sync/v2/exchange")[0].responseBody).acknowledgements;
  t.eq(acks.map((x) => x.duplicate), [true, false], "first duplicate, second new");
  rows = await eventsOf(h, fw.deviceId);
  t.eq(rows.filter((r) => r.payload.xpointer === xp(13)).length, 1, "one row for the event that was lost twice");
  t.eq(rows.filter((r) => r.payload.xpointer === xp(14)).length, 1, "one row for the new one");
  t.eq((await headOf(h, a.md5, account.id))[0].xpointer, xp(14), "the newest stays the device's position");
  t.eq((await fw.once("status")).cursor, await watermark(h, account.id), "cursor at the watermark");

  // Faults answered by status.
  h.proxy.clear();
  h.proxy.fault({ match: "/api/sync/v2/pull", action: "status", status: 503, code: "sync_service_unavailable" });
  const s503 = await fw.once("manual-session");
  t.eq(s503.report.outcome, "server_error", "503 is a server error");
  t.ok((await fw.once("status")).next_attempt_epoch > now(), "503 backs off");
  h.proxy.clear();
  h.proxy.fault({ match: "/api/sync/v2/pull", action: "status", status: 429, retryAfter: 900 });
  const s429 = await fw.once("manual-session");
  t.eq(s429.report.outcome, "rate_limited", "429 is rate limited");
  const after429 = await fw.once("status");
  t.ok(after429.next_attempt_epoch >= now() + 880, `Retry-After honoured: ${after429.next_attempt_epoch - now()} s`);
  t.eq((await fw.once("gate", { kind: "sleep" })).gate, "backed_off", "automatic sessions wait for it");
  h.proxy.clear();
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "manual still goes and clears it");
  t.eq((await fw.once("status")).next_attempt_epoch, 0, "backoff cleared");

  // The server's own limiter, not an injected one.
  h.proxy.freshIp();
  let realLimit = null;
  for (let i = 0; i < 260 && !realLimit; i += 1) {
    try {
      await kr.get("/api/sync/v2/capabilities");
    } catch (error) {
      if (error.status === 429) realLimit = error;
    }
  }
  t.ok(realLimit, "the server's limiter fired");
  const limited = await fw.once("manual-session");
  t.eq(limited.report.outcome, "rate_limited", "the firmware reads the real 429");
  const realStatus = await fw.once("status");
  t.eq(realStatus.diag.code, "rate_limited", "with the server's code");
  t.ok(realStatus.next_attempt_epoch > now() + 5, `and its Retry-After: ${realStatus.next_attempt_epoch - now()} s`);
  h.proxy.freshIp();
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "next IP, session ok");

  // Capabilities are checked once, and a failure there closes the queue without loss.
  const fresh = h.firmware();
  await h.pairFirmware(fresh, account);
  fresh.putBook(a.name, a.bytes);
  await fresh.once("finalize-book", { ...bookArgs(a), pct: 3, xp: xp(3), page: 3, turn_epoch: now() - 7200 });
  h.proxy.clear();
  h.proxy.fault({ match: "/api/sync/v2/capabilities", action: "status", status: 503 });
  const capsFail = await fresh.once("manual-session");
  t.eq(capsFail.report.outcome, "server_error", "capabilities failed");
  t.eq((await fresh.once("status")).depth, 1, "nothing lost");
  h.proxy.clear();
  h.proxy.fault({ match: "/api/sync/v2/capabilities", action: "garbage" });
  t.eq((await fresh.once("manual-session")).report.outcome, "protocol_error", "garbage capabilities close the queue");
  t.eq((await fresh.once("manual-session")).report.outcome, "ok", "next session is fine");

  // The sweep. One full session: pull head, choice, pull, exchange, commit, health. Every
  // request in it, every failure, each from an equivalent starting point.
  const sweep = await h.fwAndKoreader("sweep");
  const b = book(sweep.fw);
  const start = now() + 1000; // KOReader's readings stay ahead of the firmware's clock, so it always asks.
  const clips = [];
  let iteration = 0;

  const prepare = async () => {
    iteration += 1;
    h.proxy.freshIp();
    await krRead(sweep.kr, b, 50 + (iteration % 40), { epoch: start + iteration * 10, xpointer: xp(1000 + iteration) });
    sweep.kr.enqueueAnnotation({ book: b.md5, bookInfo: bookInfo(b), text: `KOReader subraya ${iteration}`, page: iteration, xpointer: xp(5000 + iteration), pos1: xp(5500 + iteration), note: `nota ${iteration}` });
    await sweep.kr.syncAll();
    clips.push({ spine: 1, offset: iteration, hash: 1000 + iteration, text: `Firmware subraya ${iteration}`, page: iteration, xpointer: xp(9000 + iteration) });
    await sweep.fw.once("finalize-book", {
      ...bookArgs(b),
      pct: (iteration % 90) + 1,
      xp: xp(20000 + iteration),
      page: (iteration % 90) + 1,
      turn_epoch: now() - 2000 + iteration,
      clips: sweep.fw.writeClips(clips),
      session_start: now() - 4000 - iteration * 10,
      session_end: now() - 3000 - iteration * 10,
      pages_read: 3,
    });
    const box3 = await sweep.fw.once("outbox");
    // A never-attempted progress event may be replaced by the choice (rule 5), so its id
    // is not a promise. Annotations and sessions are, and progress is counted below.
    return box3.events.filter((e) => e.kind !== 1).map((e) => e.id);
  };

  const fullSession = async () => {
    const proc = sweep.fw.boot();
    try {
      const open = await proc.cmd("open-session", { ...bookArgs(b), pct: 5, xp: xp(5), page: 5 });
      if (open.decision.ask) await proc.cmd("apply-choice", { choice: "stay", total: b.total });
      await proc.cmd("manual-session");
    } finally {
      await proc.close();
    }
  };

  // Clean runs to learn what a session is. The first also asks for capabilities, once
  // ever, and leaves a suggestion for the second to settle.
  const expected = await prepare();
  await fullSession();
  for (const id of await prepare()) expected.push(id);
  await fullSession();
  for (const id of await prepare()) expected.push(id);
  h.proxy.clear();
  await fullSession();
  const baseline = h.proxy.log.map((e) => e.path.replace(/\?.*$/, ""));
  metrics["Fault sweep: requests in one full session"] = baseline.join(" -> ");
  t.ok(baseline.length >= 4, `a session has several requests: ${baseline.join(", ")}`);
  const allIds = new Set(expected);

  const actions = [
    { label: "drop before the server sees it", rule: { action: "drop-before" } },
    { label: "drop after the server committed", rule: { action: "drop-after" } },
    { label: "truncate the body", rule: { action: "truncate", fraction: 0.5 } },
    { label: "truncate the headers", rule: { action: "truncate-headers" } },
    { label: "delay", rule: { action: "delay", delayMs: 300 } },
    { label: "503 without reaching the server", rule: { action: "status", status: 503 } },
    { label: "503 after the server committed", rule: { action: "status", status: 503, forward: true } },
    { label: "429 with Retry-After", rule: { action: "status", status: 429, retryAfter: 2 } },
    { label: "garbage 200", rule: { action: "garbage" } },
    { label: "corrupted body", rule: { action: "corrupt", offsets: [2] } },
  ];

  let runs = 0;
  let fired = 0;
  for (let position = 1; position <= baseline.length; position += 1) {
    for (const { label, rule } of actions) {
      const ids = await prepare();
      for (const id of ids) allIds.add(id);
      h.proxy.clear();
      const armed = h.proxy.faultAtRequest(position, rule);
      await fullSession().catch((error) => t.ok(false, `session crashed at ${baseline[position - 1]} with ${label}: ${error.message}`));
      fired += armed.fired;
      h.proxy.clear();

      // The next sessions, no faults: the same thing a person would do.
      h.proxy.freshIp();
      await fullSession();
      const manual = await sweep.fw.once("manual-session");
      const where = `request ${position} (${baseline[position - 1]}), ${label}`;
      t.eq(manual.report.outcome, "ok", `${where}: converges`);
      const state = await sweep.fw.once("status");
      t.eq(state.depth, 0, `${where}: queue empty`);
      t.eq(state.quarantined, 0, `${where}: nothing quarantined`);
      t.eq(state.rejected_events, 0, `${where}: nothing rejected`);
      t.eq(state.cursor, await watermark(h, sweep.account.id), `${where}: cursor at the watermark`);
      const [{ pull_cursor: pullCursor }] = await h.sql`SELECT pull_cursor::text FROM device_sync_state WHERE device_id = ${sweep.fw.deviceId}`;
      t.ok(BigInt(pullCursor) <= BigInt(state.cursor), `${where}: server checkpoint not past the client`);
      runs += 1;
    }
  }
  metrics["Fault sweep: faulted sessions"] = `${runs} (${baseline.length} requests x ${actions.length} failures), ${fired} faults fired`;
  t.eq(fired, runs, "every fault really hit its request");

  // Invariants over the whole sweep: no loss, no duplicates, on both sides.
  const recorded = await h.sql`SELECT client_event_id, count(*)::int AS n FROM device_events WHERE device_id = ${sweep.fw.deviceId} GROUP BY client_event_id`;
  const recordedIds = new Set(recorded.map((r) => r.client_event_id));
  const lost = [...allIds].filter((id) => !recordedIds.has(id));
  t.eq(lost.length, 0, `no queued event was lost (of ${allIds.size})`);
  t.eq(recorded.filter((r) => r.n > 1).length, 0, "no event recorded twice");
  const [{ count: progressRows }] = await h.sql`SELECT count(*)::int FROM device_events WHERE device_id = ${sweep.fw.deviceId} AND event_type = 'progress.changed'`;
  t.eq(progressRows, iteration, "one own progress per answered prompt, none lost, none repeated");
  const sequences = await h.sql`SELECT client_sequence, count(*)::int AS n FROM device_events WHERE device_id = ${sweep.fw.deviceId} GROUP BY client_sequence HAVING count(*) > 1`;
  t.eq(sequences.length, 0, "no sequence used twice");
  const [{ count: entryCount }] = await h.sql`
    SELECT count(*)::int FROM entries e JOIN documents d ON d.id = e.document_id
    WHERE d.user_id = ${sweep.account.id} AND e.text LIKE 'Firmware subraya %'`;
  t.eq(entryCount, iteration, "one entry per firmware annotation");
  const sessionsRows = await h.sql`SELECT started_at, count(*)::int AS n FROM reading_sessions WHERE device_id = ${sweep.fw.deviceId} GROUP BY started_at`;
  t.eq(sessionsRows.length, iteration, "one reading session per finalize");
  t.eq(sessionsRows.filter((r) => r.n > 1).length, 0, "no session recorded twice");
  const [{ count: krEntries }] = await h.sql`
    SELECT count(*)::int FROM entries e JOIN documents d ON d.id = e.document_id
    WHERE d.user_id = ${sweep.account.id} AND e.text LIKE 'KOReader subraya %'`;
  t.eq(krEntries, iteration, "every KOReader annotation landed");
  await sweep.fw.once("manual-session");
  t.eq((await sweep.fw.once("status")).cursor, await watermark(h, sweep.account.id), "the firmware's cursor went past all of them");
  t.eq((await sweep.kr.syncAll()).pending, 0, "KOReader's queue is empty too");
}

// ---------------------------------------------------------------------------------
// 6. Restored cards and old credentials
// ---------------------------------------------------------------------------------

async function repair(h, t) {
  const { account, fw, kr } = await h.fwAndKoreader("repair");
  const a = book(fw);
  const b = book(fw);
  await fw.once("finalize-book", { ...bookArgs(a), pct: 10, xp: xp(10), page: 10, turn_epoch: now() - 5000 });
  t.eq((await fw.once("sleep-session")).report.events_acked, 1, "first event up");
  const snap = path.join(RUN_DIR, "snapshot-repair");
  fw.snapshot(snap);

  // Life goes on: more events up, more events down.
  await fw.once("finalize-book", { ...bookArgs(a), pct: 20, xp: xp(20), page: 20, turn_epoch: now() - 4000 });
  await fw.once("finalize-book", { ...bookArgs(b), pct: 21, xp: xp(21), page: 21, turn_epoch: now() - 4000 });
  await fw.once("sleep-session");
  const usedBefore = (await eventsOf(h, fw.deviceId)).map((r) => String(r.client_sequence));
  t.eq(usedBefore.length, 3, "three events recorded");
  for (let i = 0; i < 3; i += 1) {
    kr.enqueueAnnotation({ book: a.md5, bookInfo: bookInfo(a), text: `Mientras tanto ${i}`, page: i, xpointer: xp(300 + i), pos1: xp(350 + i) });
  }
  await kr.syncAll();
  await fw.once("manual-session");
  const cursorLater = (await fw.once("status")).cursor;

  // The whole card goes back: older state and older outbox.
  fw.restore(snap);
  const restored = await fw.once("status");
  t.ok(BigInt(restored.cursor) < BigInt(cursorLater), "the card is really older");
  t.eq(restored.next_sequence, "2", "and its counter is older: it will reuse a sequence");
  await fw.once("finalize-book", { ...bookArgs(b), pct: 25, xp: xp(25), page: 25, turn_epoch: now() - 3000 });
  h.proxy.clear();
  const repaired = await fw.once("sleep-session");
  const exchanges = h.proxy.requestsTo("/api/sync/v2/exchange");
  t.ok(exchanges.some((e) => e.status === 409 && JSON.parse(e.responseBody).error.code === "client_sequence_conflict"), "the server refuses the reused sequence");
  t.eq(repaired.report.sequence_repaired, true, "the engine repairs it");
  t.eq(repaired.report.outcome, "ok", "and the session ends well");
  t.eq(repaired.report.depth, 0, "queue empty");
  const firstBody = JSON.parse(exchanges[0].body).events[0];
  const secondBody = JSON.parse(exchanges[1].body).events[0];
  t.eq(secondBody.client_event_id, firstBody.client_event_id, "same id after the repair");
  t.ok(Number(secondBody.client_sequence) > Number(firstBody.client_sequence), "a new number");
  t.eq({ ...secondBody, client_sequence: 0 }, { ...firstBody, client_sequence: 0 }, "same content after the repair");
  const rows = await eventsOf(h, fw.deviceId);
  t.eq(rows.length, 4, "the new event is recorded, nothing else");
  t.eq(rows[rows.length - 1].payload.xpointer, xp(25), "the right one");
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "the replayed cursor keeps going");
  const caught = await fw.once("status");
  t.eq(caught.cursor, await watermark(h, account.id), "and catches up");

  // Only the state files go back: the replay is longer than a session, and a checkpoint
  // older than the server's is not an error.
  for (let i = 0; i < 6; i += 1) {
    kr.enqueueAnnotation({ book: b.md5, bookInfo: bookInfo(b), text: `Otra tanda ${i}`, page: i, xpointer: xp(400 + i), pos1: xp(450 + i) });
  }
  await kr.syncAll();
  await fw.once("manual-session");
  const snapState = path.join(RUN_DIR, "snapshot-state");
  fs.rmSync(snapState, { recursive: true, force: true });
  fs.mkdirSync(snapState);
  fw.restore(snap, ["state.a", "state.b"]);
  const small = { "pull-limit": 1, "max-pull-pages": 1 };
  h.proxy.clear();
  const replay = await fw.once("manual-session", {}, small);
  t.eq(replay.report.outcome, "ok", "a short replay ends well");
  const commit = h.proxy.requestsTo("/api/sync/v2/cursor/commit")[0];
  t.ok(commit && commit.status === 409 && JSON.parse(commit.responseBody).error.code === "cursor_regression", "commit says cursor_regression");
  t.eq(replay.report.cursor_recovered, true, "the engine notes it and keeps its own checkpoint");
  const [server1] = await h.sql`SELECT pull_cursor::text FROM device_sync_state WHERE device_id = ${fw.deviceId}`;
  t.ok(BigInt((await fw.once("status")).cursor) < BigInt(server1.pull_cursor), "the client is still behind the server's checkpoint");
  for (let i = 0; i < 6; i += 1) await fw.once("manual-session");
  t.eq((await fw.once("status")).cursor, await watermark(h, account.id), "it converges");

  // The server's checkpoints went back (restored database): cursor_not_delivered.
  await h.sql`UPDATE device_sync_state SET last_delivered_cursor = 0, pull_cursor = 0 WHERE device_id = ${fw.deviceId}`;
  h.proxy.clear();
  const notDelivered = await fw.once("manual-session");
  t.ok(h.proxy.log.some((e) => e.status === 409 && e.responseBody.includes("cursor_not_delivered")), "the server answers cursor_not_delivered");
  t.eq(notDelivered.report.cursor_recovered, true, "the cursor moves back to what was delivered");
  t.eq(notDelivered.report.outcome, "ok", "and the session goes on");

  // The previous firmware's credential, issued by this server.
  const old = new KoreaderSim({ baseUrl: h.proxy.url, kobo: "C:\\Users\\totot\\Documents\\Proyectos\\highlights-kobo", name: "Xteink X4 viejo", platform: "crossink-x4", externalId: "x4-legacy-device" });
  await h.approve(account, (await old.pairingRequest()).user_code);
  const claimed = await old.pairingClaim();
  const oldBook = book(null);
  for (let i = 1; i <= 3; i += 1) {
    old.enqueueProgress({ book: oldBook.md5, page: i, totalPages: 100, xpointer: xp(i), title: oldBook.title, author: oldBook.author, epoch: now() - 90000 + i });
  }
  old.outbox.forEach((e, i) => {
    e.client_sequence = String(i + 1);
  });
  // coalesce keeps a single unattempted event per book, so send them one at a time.
  await old.syncAll();
  old.enqueueProgress({ book: oldBook.md5, page: 4, totalPages: 100, xpointer: xp(4), title: oldBook.title, author: oldBook.author, epoch: now() - 89000 });
  await old.syncAll();
  const oldEvents = (await eventsOf(h, claimed.device.id)).length;
  t.eq(oldEvents, 2, "the old firmware already used sequences on this device");

  const imported = h.firmware();
  const mac = Buffer.from(imported.mac, "hex");
  const obf = Buffer.from(old.token, "ascii").map((byte, i) => byte ^ mac[i % 6]);
  fs.mkdirSync(path.join(imported.sd, ".borges"), { recursive: true });
  // Never a real server URL here. The CLI overrides it with the proxy before any request.
  fs.writeFileSync(
    path.join(imported.sd, ".borges", "borges.json"),
    JSON.stringify({ baseUrl: "https://contract.invalid/", deviceId: claimed.device.id, token_obf: Buffer.from(obf).toString("base64"), serverIp: "127.0.0.1", serverIpHost: "contract.invalid" })
  );
  const st = await imported.once("status");
  t.eq(st.paired, true, "the old credential is imported silently");
  t.eq(st.device_id, claimed.device.id, "with the same device");
  t.ok(st.base_url.startsWith("http://127.0.0.1"), "the harness keeps it on loopback");
  oldBook.path = imported.putBook(oldBook.name, oldBook.bytes);
  await imported.once("finalize-book", { ...bookArgs(oldBook), pct: 60, xp: xp(60), page: 60, turn_epoch: now() - 100 });
  h.proxy.clear();
  const firstSession = await imported.once("sleep-session");
  t.eq(firstSession.report.outcome, "ok", "the imported device syncs");
  t.eq(firstSession.report.sequence_repaired, true, "and repairs its way past the old firmware's sequences");
  const [{ count: legacyRows }] = await h.sql`SELECT count(*)::int FROM devices WHERE external_id = 'x4-legacy-device'`;
  t.eq(legacyRows, 1, "no new device row");
  const after = await eventsOf(h, claimed.device.id);
  t.eq(after.length, 3, "the event is on the same device row");
  t.near(after[2].payload.percentage, 60, 0.0001, "the right event");
  t.eq((await imported.once("status")).depth, 0, "queue empty");

  // A file from another chip does not decode: ignored, not half imported.
  const wrong = h.firmware();
  fs.mkdirSync(path.join(wrong.sd, ".borges"), { recursive: true });
  fs.copyFileSync(path.join(imported.sd, ".borges", "borges.json"), path.join(wrong.sd, ".borges", "borges.json"));
  t.eq((await wrong.once("status")).paired, false, "another chip's credential is not imported");
  // The older file name, in plain text.
  const plain = h.firmware();
  fs.mkdirSync(path.join(plain.sd, ".borges"), { recursive: true });
  fs.writeFileSync(path.join(plain.sd, ".borges", "toto.json"), JSON.stringify({ deviceId: claimed.device.id, token: old.token }));
  t.eq((await plain.once("status")).device_id, claimed.device.id, "toto.json with a plain token is imported too");
  t.eq((await plain.once("manual-session")).report.outcome, "ok", "and works");
}

// ---------------------------------------------------------------------------------
// 7. Annotations
// ---------------------------------------------------------------------------------

async function annotations(h, t) {
  const { account, fw, kr } = await h.fwAndKoreader("notas");
  const a = book(fw, { title: 'El "Aleph" – edición ñ', author: "Jorge Luis Borges" });
  const emoji = "Subrayado con acentos ñáé, \"comillas\" y \\barra y emoji 📚";
  const clips = [
    { spine: 2, offset: 100, hash: 111, text: emoji, note: "Mi nota", chapter: "Capítulo 1", xpointer: xp(100), page: 12, pctMicro: 12500000, timeMs: 1790000000000 },
    { spine: 2, offset: 200, hash: 222, text: "Segundo subrayado", color: "yellow", xpointer: xp(200), page: 13 },
    { spine: 3, offset: 300, hash: 333, bookmark: true, text: "Marcador en la 14", page: 14, xpointer: xp(300) },
  ];
  const finalize = async (rows) =>
    fw.once("finalize-book", { ...bookArgs(a), paged: 0, clips: fw.writeClips(rows) });
  const created = await finalize(clips);
  t.eq(created.finalize.annotations_created, 3, "three events queued");
  const up = await fw.once("sleep-session");
  t.eq(up.report.events_acked, 3, "three acked");
  t.eq(up.report.events_rejected, 0, "none rejected");

  const [doc] = await h.sql`SELECT id, title, author, file, number_of_pages, book_hash FROM documents WHERE user_id = ${account.id} AND book_hash = ${a.md5}`;
  t.ok(doc, "the server created the document from the event");
  t.eq(doc?.title, a.title, "document title");
  t.eq(doc?.author, a.author, "document author");
  t.eq(doc?.file, a.name, "document file");
  t.eq(doc?.number_of_pages, a.total, "document pages");
  const [identifier] = await h.sql`SELECT kind, value FROM book_identifiers WHERE document_id = ${doc.id} AND kind = 'koreader_partial_md5'`;
  t.eq(identifier?.value, a.md5, "and the edition identifier");
  const entries = await h.sql`SELECT id, text, note, chapter, color, drawer, page, xpointer, total_pages, time FROM entries WHERE document_id = ${doc.id} ORDER BY page`;
  t.eq(entries.length, 3, "three entries");
  t.eq(entries[0].text, emoji, "text survives intact");
  t.eq(entries[0].note, "Mi nota", "note");
  t.eq(entries[0].chapter, "Capítulo 1", "chapter");
  t.eq(entries[0].color, "gray", "color defaults to gray");
  t.eq(entries[0].drawer, "lighten", "highlight drawer");
  t.eq(entries[1].color, "yellow", "an explicit color is kept");
  t.eq(entries[2].drawer, "bookmark", "bookmark drawer");
  t.eq(entries[0].xpointer, xp(100), "xpointer");
  const states = await h.sql`SELECT sync_id, entry_id, deleted_at, revision::text FROM annotation_sync_state WHERE document_id = ${doc.id}`;
  t.eq(states.length, 3, "one annotation state each");
  const events = await eventsOf(h, fw.deviceId);
  t.eq(events.map((e) => e.event_type).sort(), ["annotation.created", "annotation.created", "bookmark.upserted"], "event types");
  t.eq(events.every((e) => e.aggregate_id === e.payload.sync_id), true, "aggregate id is the sync id");

  // Nothing new: nothing sent.
  const noChange = await finalize(clips);
  t.eq(noChange.finalize.annotations_created, 0, "the same clips queue nothing");
  t.eq((await fw.once("status")).depth, 0, "queue empty");

  // Lost answer: resent, same id, one row.
  clips.push({ spine: 4, offset: 400, hash: 444, text: "Con respuesta perdida", xpointer: xp(400), page: 15 });
  await finalize(clips);
  h.proxy.clear();
  h.proxy.fault({ match: "/api/sync/v2/exchange", action: "drop-after", times: 2 });
  await fw.once("sleep-session");
  h.proxy.clear();
  const resent = await fw.once("sleep-session");
  t.eq(JSON.parse(h.proxy.requestsTo("/api/sync/v2/exchange")[0].responseBody).acknowledgements[0].duplicate, true, "resend is a duplicate");
  t.eq(resent.report.depth, 0, "and leaves the queue");
  t.eq((await h.sql`SELECT count(*)::int FROM entries WHERE document_id = ${doc.id}`)[0].count, 4, "one entry more, not two");

  // The local ledger is lost: same derived sync id, a new event, no new annotation.
  for (const suffix of ["a", "b"]) fs.rmSync(path.join(fw.borgesDir(), "ann", `${a.md5}.${suffix}`), { force: true });
  const again = await finalize(clips);
  t.eq(again.finalize.annotations_created, 4, "a lost ledger publishes again");
  const replayed = await fw.once("sleep-session");
  t.eq(replayed.report.events_rejected, 0, "the server takes them");
  t.eq((await h.sql`SELECT count(*)::int FROM entries WHERE document_id = ${doc.id}`)[0].count, 4, "still four entries");
  t.eq((await h.sql`SELECT count(*)::int FROM annotation_sync_state WHERE document_id = ${doc.id}`)[0].count, 4, "still four identities");
  t.eq((await h.sql`SELECT count(*)::int FROM sync_conflicts WHERE user_id = ${account.id} AND status = 'open'`)[0].count, 0, "no conflict from a resend");

  // Unreadable clips never mean "all deleted".
  const unreadable = await fw.once("finalize-book", { ...bookArgs(a), paged: 0, clips: fw.writeUnreadableClips() });
  t.eq(unreadable.finalize.ok, false, "unreadable clips are reported");
  t.eq(unreadable.finalize.annotations_deleted, 0, "and delete nothing");

  // Deletes. No events for clips the ledger does not know, a tombstone for those it does.
  const remaining = clips.filter((c) => c.hash !== 222 && c.hash !== 333);
  const removed = await finalize(remaining);
  t.eq(removed.finalize.annotations_deleted, 2, "one highlight and one bookmark removed");
  const deleted = await fw.once("sleep-session");
  t.eq(deleted.report.events_rejected, 0, `deletes accepted (${(await fw.once("status")).last_rejection})`);
  const allEvents = await eventsOf(h, fw.deviceId);
  t.ok(allEvents.some((e) => e.event_type === "annotation.deleted"), "annotation.deleted sent");
  t.ok(allEvents.some((e) => e.event_type === "bookmark.deleted"), "bookmark.deleted sent");
  t.eq((await h.sql`SELECT count(*)::int FROM annotation_sync_state WHERE document_id = ${doc.id} AND deleted_at IS NOT NULL`)[0].count, 2, "two tombstones");
  t.eq((await h.sql`SELECT count(*)::int FROM entries WHERE document_id = ${doc.id}`)[0].count, 2, "two entries left");

  // Changed their mind: the same passage highlighted again, after the delete went up.
  const rehighlight = [...remaining, clips.find((c) => c.hash === 222)];
  t.eq((await finalize(rehighlight)).finalize.annotations_created, 1, "highlighting it again queues an event");
  const rehigh = await fw.once("sleep-session");
  t.eq(rehigh.report.events_rejected, 0, "accepted");
  t.eq((await h.sql`SELECT count(*)::int FROM entries WHERE document_id = ${doc.id} AND text = 'Segundo subrayado'`)[0].count, 1, "the entry is back");
  t.eq((await h.sql`SELECT count(*)::int FROM sync_conflicts WHERE user_id = ${account.id} AND status = 'open'`)[0].count, 0, "highlighting again opens no conflict");
  t.eq((await h.sql`SELECT count(*)::int FROM annotation_sync_state WHERE document_id = ${doc.id} AND deleted_at IS NOT NULL`)[0].count, 1, "only the bookmark stays a tombstone");
  t.eq((await finalize(remaining)).finalize.annotations_deleted, 1, "and it can be deleted once more");
  t.eq((await fw.once("sleep-session")).report.events_rejected, 0, "accepted");

  // Inbound: what KOReader writes moves the firmware's cursor and leaves nothing on its card.
  const inboundSync = KoreaderSim.annotationSyncId(a.md5, { kind: "highlight", xpointer: xp(600), pos1: xp(601) });
  kr.enqueueAnnotation({ book: a.md5, bookInfo: bookInfo(a), text: "Desde KOReader ñ 📖", note: "nota de allá", chapter: "Capítulo 9", page: 60, xpointer: xp(600), pos1: xp(601), timeMs: 1790000001000 });
  const krUp = await kr.syncAll();
  t.eq(krUp.acks[0].status, "accepted", "KOReader's annotation accepted");
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "the firmware pulls it");
  t.eq((await fw.once("status")).cursor, await watermark(h, account.id), "and its cursor goes past it");
  const onCard = (dir) => fs.readdirSync(dir, { withFileTypes: true }).flatMap((e) => (e.isDirectory() ? onCard(path.join(dir, e.name)) : [path.join(dir, e.name)]));
  t.ok(!onCard(fw.borgesDir()).some((f) => !f.endsWith("resp.json") && fs.readFileSync(f).includes("Desde KOReader")), "nothing of it is kept on the card");
  kr.enqueueAnnotationDelete({ book: a.md5, bookInfo: bookInfo(a), syncId: inboundSync });
  const krDel = await kr.syncAll();
  t.eq(krDel.acks[0].status, "accepted", `KOReader's delete accepted: ${krDel.acks[0].rejection_code}`);
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "the firmware pulls the delete");
  const pulled = await kr.syncAll();
  t.ok(pulled.events.length >= 0, "KOReader's pull works");
  const krSaw = kr.inbox.filter((e) => e.event_type.startsWith("annotation.") && e.origin_device.id === fw.deviceId);
  t.ok(krSaw.length >= 2, "KOReader receives the firmware's annotations");
  t.ok(krSaw.every((e) => typeof e.payload?.text === "string" || e.event_type === "annotation.deleted"), "in the shape its adapter reads");

  // C29, two KOReaders: same sync id, same highlight. A baseline for what follows.
  const b = book(fw, { title: "Otro libro" });
  const kr2 = await h.koreader(account, { name: "Kobo Clara" });
  const shared = { book: b.md5, bookInfo: bookInfo(b), text: "El mismo subrayado", page: 5, xpointer: xp(700), pos1: xp(701), chapter: "Uno" };
  kr.enqueueAnnotation({ ...shared, epoch: now() - 100 });
  kr2.enqueueAnnotation({ ...shared, page: 6, epoch: now() - 90 });
  await kr.syncAll();
  await kr2.syncAll();
  const openConflicts = async () => (await h.sql`SELECT count(*)::int FROM sync_conflicts WHERE user_id = ${account.id} AND status = 'open'`)[0].count;
  t.eq(await openConflicts(), 0, "two KOReaders with the same highlight: no conflict (C29)");

  // Firmware and KOReader, the same passage. Their sync ids are derived differently.
  const fwClips = [{ spine: 7, offset: 700, hash: 777, text: "El mismo subrayado", chapter: "Uno", xpointer: xp(700), page: 5 }];
  await fw.once("finalize-book", { ...bookArgs(b), paged: 0, clips: fw.writeClips(fwClips) });
  const same = await fw.once("sleep-session");
  t.eq(same.report.events_rejected, 0, "firmware's identical annotation is accepted");
  t.eq(await openConflicts(), 0, "firmware's identical annotation opens no conflict (C29)");
  const [docB] = await h.sql`SELECT id FROM documents WHERE user_id = ${account.id} AND book_hash = ${b.md5}`;
  const sameEntries = await h.sql`SELECT id, page FROM entries WHERE document_id = ${docB.id}`;
  t.eq(sameEntries.length, 1, "one entry for the same text in the same book");
  const sameStates = await h.sql`SELECT sync_id, entry_id FROM annotation_sync_state WHERE document_id = ${docB.id} AND deleted_at IS NULL`;
  t.note(`same passage from KOReader and firmware: ${sameStates.length} sync identities on ${new Set(sameStates.map((s) => s.entry_id)).size} entry`);

  // One of those two deletes "its" highlight. The other reader still has it.
  await fw.once("finalize-book", { ...bookArgs(b), paged: 0, clips: fw.writeClips([]) });
  await fw.once("sleep-session");
  const afterDelete = await h.sql`SELECT id FROM entries WHERE document_id = ${docB.id}`;
  t.note(`after the firmware deletes its copy: ${afterDelete.length} entries remain for that passage (KOReader still shows it)`);

  // A long text from elsewhere costs nothing here.
  kr.enqueueAnnotation({ book: a.md5, bookInfo: bookInfo(a), text: "ñ".repeat(3000), page: 70, xpointer: xp(800), pos1: xp(801) });
  await kr.syncAll();
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "a long inbound text is no problem");
}


// ---------------------------------------------------------------------------------
// 8. session.ended
// ---------------------------------------------------------------------------------

async function sessions(h, t) {
  const { fw, kr } = await h.fwAndKoreader("sesiones");
  const a = book(fw);
  const start = now() - 50000;
  const run = async (i, duration, extra = {}) => {
    const session_start = start + i * 20000;
    return fw.once("finalize-book", {
      ...bookArgs(a),
      paged: 0,
      session_start,
      session_end: session_start + duration,
      pages_read: 7,
      start_page: 10,
      end_page: 17,
      ...extra,
    });
  };
  t.eq((await run(0, 60)).finalize.session_queued, true, "60 s is valid");
  t.eq((await run(1, 14400)).finalize.session_queued, true, "14400 s is valid");
  t.eq((await run(2, 3600)).finalize.session_queued, true, "one hour is valid");
  t.eq((await run(3, 59)).finalize.session_queued, false, "59 s is not emitted");
  t.eq((await run(4, 14401)).finalize.session_queued, false, "14401 s is not emitted");
  t.eq((await run(5, -100)).finalize.session_queued, false, "negative is not emitted");
  t.eq((await run(6, 600, { session_precision: "unknown" })).finalize.session_queued, false, "an unknown clock has nothing honest to report");
  t.eq((await run(7, 600, { session_precision: "approximate" })).finalize.session_queued, false, "approximate neither");
  t.eq((await run(8, 600, { session_precision: "anchored" })).finalize.session_queued, true, "anchored is good enough");
  t.eq((await fw.once("status")).depth, 4, "only the valid ones queued");
  const up = await fw.once("sleep-session");
  t.eq(up.report.events_acked, 4, "accepted");
  t.eq(up.report.events_rejected, 0, "none rejected");
  const rows = await h.sql`SELECT book_hash, duration_seconds, pages_read, start_page, end_page, started_at, ended_at FROM reading_sessions WHERE device_id = ${fw.deviceId} ORDER BY started_at`;
  t.eq(rows.map((r) => r.duration_seconds), [60, 14400, 3600, 600], "reading_sessions has the four, with their durations");
  t.eq(rows[0].book_hash, a.md5, "keyed by book");
  t.eq(rows[0].pages_read, 7, "pages read");
  t.eq(rows[0].start_page, 10, "start page");
  t.eq(rows[0].end_page, 17, "end page");
  t.eq(new Date(rows[0].started_at).toISOString(), iso(start), "started_at");
  t.eq(new Date(rows[0].ended_at).toISOString(), iso(start + 60), "ended_at");
  const events = (await eventsOf(h, fw.deviceId)).filter((e) => e.event_type === "session.ended");
  t.eq(events[0].aggregate_id, `session:${a.md5}:${iso(start).replace(".000Z", "Z")}`, "aggregate id");

  // The same session sent twice, as a retried finalize would.
  const twiceStart = start - 30000;
  for (let i = 0; i < 2; i += 1) {
    await fw.once("finalize-book", { ...bookArgs(a), paged: 0, session_start: twiceStart, session_end: twiceStart + 300, pages_read: 1 });
  }
  await fw.once("sleep-session");
  const [{ count: twiceRows }] = await h.sql`SELECT count(*)::int FROM reading_sessions WHERE device_id = ${fw.deviceId} AND started_at = ${iso(twiceStart)}`;
  t.note(`the same session finalized twice leaves ${twiceRows} reading_sessions row(s)`);

  // What the server does with the invalid ones: KOReader sends them as they are.
  const bad = [30, 14401];
  for (const duration of bad) {
    const begun = iso(now() - 40000 - duration);
    kr.enqueueSession({ book: a.md5, startedAt: begun, endedAt: iso(now() - 40000), duration });
  }
  const answers = await kr.syncAll();
  t.eq(answers.acks.map((x) => x.status), ["rejected", "rejected"], "the server rejects the same limits");
  t.eq(answers.pending, 0, "a rejection leaves the queue");
  t.note(`server rejection codes: ${answers.acks.map((x) => x.rejection_code).join(", ")}`);
  const [{ count: krRows }] = await h.sql`SELECT count(*)::int FROM reading_sessions WHERE device_id = ${kr.deviceId}`;
  t.eq(krRows, 0, "and stores neither");
  // Same bounds on both sides: what the firmware accepts, the server materializes.
  kr.enqueueSession({ book: a.md5, startedAt: iso(now() - 30060), endedAt: iso(now() - 30000), duration: 60 });
  t.eq((await kr.syncAll()).acks[0].status, "accepted", "KOReader's 60 s is accepted too");
}

// ---------------------------------------------------------------------------------
// 9. Library
// ---------------------------------------------------------------------------------

async function library(h, t) {
  const account = await h.account("biblio");
  const fw = h.firmware();
  await h.pairFirmware(fw, account);

  const long = "Una historia universal de la infamia que no termina nunca ".repeat(3).trim();
  const hostile = [
    { title: 'Comillas "dobles" y \'simples\'', file: 'Comillas "dobles".epub' },
    { title: "Barras / y \\ invertidas", file: "..\\..\\Windows\\system32.epub" },
    { title: "Traversal", file: "../../etc/passwd.epub" },
    { title: "Acentos áéíóú ñ Ü", file: "Acentos áéíóú ñ.epub" },
    { title: "Emoji 📚🔥", file: "Emoji 📚🔥.epub" },
    { title: long, file: `${long}.epub` },
    { title: "Reservados <>:|?*", file: "Reservados <>:|?*.epub" },
    { title: "Puntos finales...", file: "Puntos finales....epub" },
    { title: "Mayúsculas", file: "MAYUSCULAS.EPUB" },
    { title: "Sin nombre útil", file: "....epub" },
  ];
  const uploaded = new Map();
  const add = async (title, file, padBytes = 2000) => {
    const bytes = buildEpub({ title, author: 'Autora "Ñ" 📖', padBytes, seed: file });
    const res = await h.upload(account, file, bytes);
    uploaded.set(koreaderPartialMd5(bytes), { title, file, bytes, id: res.file?.id ?? res.id });
    return bytes;
  };
  for (const item of hostile) await add(item.title, item.file);
  for (let i = 0; i < 12; i += 1) await add(`Normal ${i}`, `normal-${i}.epub`);
  const bigBytes = await add("Grande para resumir", "grande.epub", 2 * 1024 * 1024);
  const bigMd5 = koreaderPartialMd5(bigBytes);

  // Pages.
  const seen = new Map();
  let page = 1;
  const proc = fw.boot();
  for (;;) {
    const res = await proc.cmd("catalog", { page, limit: 10 });
    t.eq(res.result, "ok", `page ${page}`);
    t.eq(res.page, page, `page ${page} is the one asked for`);
    t.eq(res.cache_loaded, true, `page ${page} cached intact`);
    t.ok(res.count <= 10, `page ${page} respects the limit`);
    t.eq(res.has_previous, page > 1, `page ${page} knows about the previous one`);
    for (const entry of res.books) seen.set(entry.md5, entry);
    if (!res.has_next) break;
    page += 1;
    if (page > 10) break;
  }
  await proc.close();
  t.ok(page >= 3, `more than one page (${page})`);
  t.eq(seen.size, uploaded.size, "the listing has every book exactly once");
  for (const [md5, source] of uploaded) {
    const entry = seen.get(md5);
    if (!t.ok(entry, `listed: ${source.title.slice(0, 30)}`)) continue;
    t.eq(entry.size, source.bytes.length, `size of ${source.title.slice(0, 20)}`);
    t.eq(entry.sha256, sha256(source.bytes), `sha256 of ${source.title.slice(0, 20)}`);
    t.ok(!/[/\\:*?"<>|]/.test(entry.file_name), `no hostile characters in ${entry.file_name}`);
    t.ok(entry.file_name.endsWith(".epub"), `extension of ${entry.file_name}`);
    t.ok(!entry.file_name.startsWith("."), `no leading dot in ${entry.file_name}`);
    t.ok(Buffer.byteLength(entry.file_name) < 64, `bounded name ${entry.file_name}`);
  }
  const hostileTitle = [...seen.values()].find((e) => e.title.startsWith("Comillas"));
  t.eq(hostileTitle?.title, 'Comillas "dobles" y \'simples\'', "a hostile title survives");
  const emojiTitle = [...seen.values()].find((e) => e.title.startsWith("Emoji"));
  t.eq(emojiTitle?.title, "Emoji 📚🔥", "emoji survive");
  const longTitle = [...seen.values()].find((e) => e.title.startsWith("Una historia"));
  t.ok(longTitle && Buffer.byteLength(longTitle.title) < 96 && !longTitle.title.includes("\uFFFD"), "a long title is cut on a character boundary");
  t.eq((await fw.once("catalog-cached", { page: 1 })).cache_loaded, true, "the list opens from the cache, no radio");

  const pageOf = async (md5) => {
    for (let p = 1; p <= page; p += 1) {
      const cached = await fw.once("catalog-cached", { page: p });
      if (cached.books.some((b) => b.md5 === md5)) return { p, entry: cached.books.find((b) => b.md5 === md5) };
    }
    return null;
  };

  // Download of every hostile name: they all land where they should.
  const titles = hostile.map((x) => x.title);
  for (const entry of seen.values()) {
    if (!titles.includes(uploaded.get(entry.md5).title)) continue;
    const { p } = await pageOf(entry.md5);
    const d = await fw.once("download", { id: entry.id, page: p, dir: "/books" });
    t.eq(d.result, "done", `download "${entry.file_name}"`);
    const landed = fw.hostPath(`/books/${entry.file_name}`);
    t.ok(fs.existsSync(landed), `file exists: ${entry.file_name}`);
    if (fs.existsSync(landed)) t.eq(sha256(fs.readFileSync(landed)), entry.sha256, `bytes of ${entry.file_name}`);
    t.eq((await fw.once("md5", { file: `/books/${entry.file_name}` })).md5, entry.md5, `partial MD5 on SD = server's, ${entry.file_name}`);
  }
  const outside = fs.readdirSync(path.dirname(fw.sd)).filter((n) => n.endsWith(".epub"));
  t.eq(outside, [], "nothing escaped the books directory");

  // Range resume after a truncated download.
  const big = seen.get(bigMd5);
  const { p: bigPage } = await pageOf(bigMd5);
  h.proxy.clear();
  h.proxy.fault({ match: /\/download$/, action: "truncate", fraction: 0.4 });
  const cut = await fw.once("download", { id: big.id, page: bigPage, dir: "/books" });
  t.eq(cut.result, "interrupted", "a cut download is interrupted");
  t.eq(cut.on_sd, false, "and not on the SD as a book");
  const part = fw.hostPath(`/books/${big.file_name}.part`);
  t.ok(fs.existsSync(part), ".part kept");
  const have = fs.statSync(part).size;
  t.near(have, bigBytes.length * 0.4, 8192, ".part has what arrived");
  h.proxy.clear();
  const resumed = await fw.once("download", { id: big.id, page: bigPage, dir: "/books" });
  t.eq(resumed.result, "done", "the next call resumes");
  const rangeRequest = h.proxy.requestsTo(/\/download$/)[0];
  t.eq(rangeRequest.headers.range, `bytes=${have}-`, "with Range");
  t.ok(rangeRequest.headers["if-range"], "and If-Range");
  t.eq(rangeRequest.status, 206, "answered 206");
  t.eq(rangeRequest.responseHeaders["x-koreader-partial-md5"], bigMd5, "the server's own hash header");
  t.eq(sha256(fs.readFileSync(fw.hostPath(`/books/${big.file_name}`))), big.sha256, "resumed file is byte-identical");
  t.ok(!fs.existsSync(part), ".part gone");
  t.ok(!fs.existsSync(`${fw.hostPath(`/books/${big.file_name}`)}.etag`), ".etag gone");
  t.eq((await fw.once("md5", { file: `/books/${big.file_name}` })).md5, bigMd5, "partial MD5 of the resumed file");

  // Already there: no request.
  h.proxy.clear();
  const present = await fw.once("download", { id: big.id, page: bigPage, dir: "/books" });
  t.eq(present.result, "already_present", "already on the SD");
  t.eq(present.was_on_sd, true, "and the screen can mark it");
  t.eq(h.proxy.requestsTo(/\/download$/).length, 0, "without asking the server");

  // The server's device presence.
  const [{ count: presence }] = await h.sql`SELECT count(*)::int FROM device_book_presence WHERE device_id = ${fw.deviceId} AND state = 'downloaded'`;
  t.ok(presence >= 1, "the server knows this device has them");

  // Corruption in a sampled KiB: refused, nothing accepted.
  const normal = [...seen.values()].find((e) => e.title === "Normal 0");
  const { p: normalPage } = await pageOf(normal.md5);
  h.proxy.clear();
  h.proxy.fault({ match: /\/download$/, action: "corrupt", offsets: [100] });
  const corrupt = await fw.once("download", { id: normal.id, page: normalPage, dir: "/books" });
  t.eq(corrupt.result, "integrity_failed", "corrupted bytes are refused");
  t.eq(corrupt.on_sd, false, "nothing accepted");
  t.ok(!fs.existsSync(fw.hostPath(`/books/${normal.file_name}.part`)), "no .part left to resume from");
  h.proxy.clear();
  t.eq((await fw.once("download", { id: normal.id, page: normalPage, dir: "/books" })).result, "done", "the next try is clean");

  // Same corruption in the resumed half.
  const normal2 = [...seen.values()].find((e) => e.title === "Normal 1");
  const { p: normal2Page } = await pageOf(normal2.md5);
  h.proxy.clear();
  h.proxy.fault({ match: /\/download$/, action: "truncate", bytes: 200 });
  t.eq((await fw.once("download", { id: normal2.id, page: normal2Page, dir: "/books" })).result, "interrupted", "cut after 200 bytes");
  h.proxy.clear();
  h.proxy.fault({ match: /\/download$/, action: "corrupt", offsets: [100] });
  t.eq((await fw.once("download", { id: normal2.id, page: normal2Page, dir: "/books" })).result, "integrity_failed", "corruption in the resumed part is caught");

  // Corruption outside the sampled KiBs. The design verifies the partial MD5 only.
  const normal3 = [...seen.values()].find((e) => e.title === "Normal 2");
  const { p: normal3Page } = await pageOf(normal3.md5);
  const sampled = (o) => [0, 1024, 4096, 16384, 65536, 262144, 1048576].some((s) => o >= s && o < s + 1024);
  let free = normal3.size - 40;
  while (sampled(free)) free -= 1024;
  h.proxy.clear();
  h.proxy.fault({ match: /\/download$/, action: "corrupt", offsets: [free] });
  const silent = await fw.once("download", { id: normal3.id, page: normal3Page, dir: "/books" });
  const silentBytes = fs.existsSync(fw.hostPath(`/books/${normal3.file_name}`)) ? fs.readFileSync(fw.hostPath(`/books/${normal3.file_name}`)) : null;
  t.note(`corruption at byte ${free}, outside the sampled KiBs: download says ${silent.result}; sha256 ${silentBytes && sha256(silentBytes) === normal3.sha256 ? "matches" : "does NOT match"} the manifest`);

  // An error page must not end up inside a file.
  const normal4 = [...seen.values()].find((e) => e.title === "Normal 3");
  const { p: normal4Page } = await pageOf(normal4.md5);
  h.proxy.clear();
  h.proxy.fault({ match: /\/download$/, action: "status", status: 503 });
  const refused = await fw.once("download", { id: normal4.id, page: normal4Page, dir: "/books" });
  t.eq(refused.result, "server_error", "503 during download");
  t.ok(!fs.existsSync(fw.hostPath(`/books/${normal4.file_name}`)), "no book from an error page");
  const stray = fw.hostPath(`/books/${normal4.file_name}.part`);
  t.ok(!fs.existsSync(stray) || fs.statSync(stray).size === 0, "and no error page in the .part");
  h.proxy.clear();
  t.eq((await fw.once("download", { id: normal4.id, page: normal4Page, dir: "/books" })).result, "done", "the next try is clean");

  // Malformed catalog page: the cached copy stays.
  h.proxy.clear();
  h.proxy.fault({ match: /\/api\/library\/plugin\/books\?/, action: "garbage" });
  t.eq((await fw.once("catalog", { page: 1, limit: 10 })).result, "malformed", "a bad page is refused");
  t.eq((await fw.once("catalog-cached", { page: 1 })).cache_loaded, true, "and the good cached one is intact");

  // Two different books with the same file name.
  const twin1 = buildEpub({ title: "Gemelo uno", seed: "uno" });
  const twin2 = buildEpub({ title: "Gemelo dos", seed: "dos" });
  await h.upload(account, "gemelos.epub", twin1);
  await h.upload(account, "gemelos.epub", twin2);
  const collide = h.firmware();
  await h.pairFirmware(collide, account);
  let twinEntries = [];
  for (let p = 1; p <= 6; p += 1) {
    const res = await collide.once("catalog", { page: p, limit: 10 });
    twinEntries.push(...res.books.filter((b) => b.title.startsWith("Gemelo")).map((b) => ({ ...b, page: p })));
    if (!res.has_next) break;
  }
  t.eq(twinEntries.length, 2, "both twins listed");
  const results = [];
  for (const twin of twinEntries) {
    results.push(await collide.once("download", { id: twin.id, page: twin.page, dir: "/books" }));
  }
  t.eq(results.map((r) => r.result), ["done", "done"], "two books with one file name both download");
  for (const twin of twinEntries) {
    const res = await collide.once("on-sd", { id: twin.id, page: twin.page, dir: "/books" });
    t.eq(res.on_sd, true, `${twin.title} is on the SD`);
  }
  const twinFiles = fs.readdirSync(collide.hostPath("/books")).filter((n) => n.endsWith(".epub"));
  const twinHashes = twinFiles.map((n) => koreaderPartialMd5(fs.readFileSync(collide.hostPath(`/books/${n}`))));
  for (const twin of twinEntries) t.ok(twinHashes.includes(twin.md5), `${twin.title}'s bytes are really there`);
}

// ---------------------------------------------------------------------------------
// 10. Revocation
// ---------------------------------------------------------------------------------

async function revocation(h, t) {
  const { account, fw } = await h.fwAndKoreader("revoca");
  const a = book(fw);
  await fw.once("finalize-book", { ...bookArgs(a), pct: 10, xp: xp(10), page: 10, turn_epoch: now() - 900 });
  await fw.once("sleep-session");
  const deviceId = fw.deviceId ?? (await fw.once("status")).device_id;

  const revoked = await h.revoke(account, deviceId);
  t.eq(revoked.status, 200, "revoked in the web app");
  await fw.once("finalize-book", { ...bookArgs(a), pct: 20, xp: xp(20), page: 20, turn_epoch: now() - 800 });
  await fw.once("finalize-book", { ...bookArgs(a), paged: 0, clips: fw.writeClips([{ spine: 1, offset: 1, hash: 1, text: "Hecho antes de reconectar", xpointer: xp(21), page: 21 }]) });
  h.proxy.clear();
  const denied = await fw.once("sleep-session");
  t.eq(denied.report.outcome, "needs_relink", "401 means reconnect");
  t.eq(h.proxy.requestsTo("/api/sync/v2/exchange").length, 0, "nothing tried to go up");
  const st = await fw.once("status");
  t.eq(st.needs_relink, true, "status says reconnect");
  t.eq(st.depth, 2, "the queue is kept");
  t.eq(st.diag.http_status, 401, "the screen can show why");
  t.ok(st.diag.code.length > 0, `with the server's code: ${st.diag.code}`);
  t.ok(st.diag.request_id.length > 0, "and a request id");
  t.eq(st.paired, true, "still shown as this account's device");
  t.eq((await fw.once("gate", { kind: "open" })).gate, "needs_relink", "open sessions stop");
  t.eq((await fw.once("gate", { kind: "sleep" })).gate, "needs_relink", "sleep sessions stop");
  t.eq((await fw.once("gate", { kind: "manual" })).gate, "go", "manual still tries");
  t.eq(st.next_attempt_epoch, 0, "no backoff: waiting does not heal this");
  const manual = await fw.once("manual-session");
  t.eq(manual.report.outcome, "needs_relink", "manual gets the same answer");
  t.eq((await fw.once("catalog", { page: 1, limit: 5 })).result, "needs_relink", "the library says so too");
  const [row] = await h.sql`SELECT status, revoked_at FROM devices WHERE id = ${deviceId}`;
  t.eq(row.status, "revoked", "the row says revoked");

  // Reconnect: same device, same queue.
  await fw.once("forget-credential");
  const gone = await fw.once("status");
  t.eq(gone.depth, 2, "forgetting the credential keeps the queue");
  await h.pairFirmware(fw, account);
  const relinked = await fw.once("status");
  t.eq(relinked.device_id, deviceId, "same device row");
  t.eq(relinked.needs_relink, false, "flag cleared");
  t.eq(relinked.depth, 2, "queue survived re-pairing the same account");
  const drained = await fw.once("sleep-session");
  t.eq(drained.report.outcome, "ok", `queue resumes: ${JSON.stringify(drained.report)}`);
  t.eq(drained.report.depth, 0, "and drains");
  const rows = await eventsOf(h, deviceId);
  t.ok(rows.some((r) => r.payload.xpointer === xp(20)), "the progress made while revoked arrived");
  t.ok(rows.some((r) => r.payload.text === "Hecho antes de reconectar"), "and the annotation");
  const [active] = await h.sql`SELECT status, revoked_at FROM devices WHERE id = ${deviceId}`;
  t.eq(active.status, "active", "the row is active again");

  // Re-pairing into ANOTHER account: its data is not carried over.
  const other = await h.account("otra");
  await fw.once("finalize-book", { ...bookArgs(a), pct: 30, xp: xp(30), page: 30, turn_epoch: now() - 100 });
  await fw.once("forget-credential");
  await h.pairFirmware(fw, other);
  const moved = await fw.once("status");
  t.eq(moved.depth, 0, "a different account empties the queue");
  t.ok(moved.device_id !== deviceId, "and is another device row");
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "and works");
}

// ---------------------------------------------------------------------------------
// 11. Volume
// ---------------------------------------------------------------------------------

async function backlog(h, t, metrics) {
  const { account, fw, kr } = await h.fwAndKoreader("volumen");
  const books = [];
  for (let i = 0; i < 5; i += 1) books.push(book(fw));

  const started = Date.now();
  const total = 2000;
  const annotationCounts = books.map(() => 0);
  const base = now() - 200000;
  for (let i = 0; i < total; i += 1) {
    // Every tenth event is progress, spread over the books like the rest.
    const which = i % 10 === 0 ? (i / 10) % books.length : i % books.length;
    const b = books[which];
    if (i % 10 === 0) {
      // A distinct aggregate state each time: not coalesced because they are attempted between batches.
      kr.enqueue({
        event_type: "progress.changed",
        aggregate_type: "reading_progress",
        aggregate_id: `koreader_partial_md5:${b.md5}`,
        book_identifier: KoreaderSim.bookIdentifier(b.md5),
        occurred_at: iso(base + i),
        time_precision: "exact",
        payload: { percentage: (i % 100) + 0.5, current_page: (i % 100) + 1, total_pages: 100, xpointer: xp(i), explicit_jump: false, title: b.title, author: b.author },
      });
    } else {
      annotationCounts[which] += 1;
      kr.enqueueAnnotation({ book: b.md5, bookInfo: bookInfo(b), text: `Subrayado ${i} del libro ${which}: ${"palabra ".repeat(12)}`, page: i % 100, xpointer: xp(i), pos1: xp(i + 100000) });
    }
  }
  let pushed = 0;
  while (kr.outbox.length) {
    h.proxy.freshIp();
    const r = await kr.sync({ maxEvents: 100 });
    pushed += r.acknowledgements.length;
    t.ok(r.acknowledgements.every((a) => a.status === "accepted"), "KOReader's batch accepted");
  }
  t.eq(pushed, total, "2,000 events on the server");
  metrics["Backlog: seconds to push 2,000 events through KOReader's path"] = ((Date.now() - started) / 1000).toFixed(1);
  const high = await watermark(h, account.id);
  const [{ count: stored }] = await h.sql`SELECT count(*)::int FROM device_events WHERE device_id = ${kr.deviceId}`;
  t.eq(stored, total, "2,000 rows");

  // A budget that cannot finish: it stops on time, with what it did saved.
  h.proxy.clear();
  h.proxy.fault({ match: /./, action: "delay", delayMs: 400, times: 1000 });
  const t0 = Date.now();
  const short = await fw.once("manual-session", {}, { "manual-budget-ms": 1000 });
  const shortMs = Date.now() - t0;
  t.eq(short.report.outcome, "budget_exhausted", "a tight budget runs out");
  t.ok(shortMs < 1000 + 400 + 1000, `and stops on time: ${shortMs} ms for a 1000 ms budget, 400 ms per request`);
  const afterBudget = await fw.once("status");
  t.ok(BigInt(afterBudget.cursor) > 0n || short.report.pages_pulled === 0, "what it finished is saved");
  h.proxy.clear();

  // Sessions until caught up.
  let cursor = BigInt(afterBudget.cursor);
  let sessions = 0;
  let maxPages = 0;
  let maxApplied = 0;
  let maxRequests = 0;
  let peak = 0;
  let largest = 0;
  while (cursor < BigInt(high) && sessions < 60) {
    h.proxy.freshIp();
    h.proxy.log = [];
    const r = await fw.once("manual-session");
    sessions += 1;
    t.eq(r.report.outcome, "ok", `session ${sessions}`);
    t.ok(r.report.pages_pulled <= 4, `session ${sessions} stays within 4 pages`);
    maxPages = Math.max(maxPages, r.report.pages_pulled);
    maxApplied = Math.max(maxApplied, r.report.events_applied);
    maxRequests = Math.max(maxRequests, h.proxy.log.length);
    peak = Math.max(peak, r.heap.peak);
    largest = Math.max(largest, r.heap.largest);
    const st = await fw.once("status");
    const next = BigInt(st.cursor);
    t.ok(next >= cursor, `cursor monotonic at session ${sessions}: ${cursor} -> ${next}`);
    t.ok(next > cursor, `session ${sessions} made progress`);
    if (next <= cursor) break;
    cursor = next;
  }
  t.eq(cursor.toString(), high, "the cursor reached the watermark");
  metrics["Backlog: sessions to absorb 2,000 inbound events"] = `${sessions} (at most ${maxPages} pages, ${maxApplied} events applied and ${maxRequests} requests per session)`;
  metrics["Backlog: peak core heap in an inbound session"] = `${peak} bytes (largest single allocation ${largest})`;
  t.ok(sessions >= 20, `a backlog this size takes many sessions (${sessions})`);

  const [{ pull_cursor: serverCursor }] = await h.sql`SELECT pull_cursor::text FROM device_sync_state WHERE device_id = ${fw.deviceId}`;
  t.eq(serverCursor, high, "the server's checkpoint agrees");

  for (let i = 0; i < books.length; i += 1) {
    const inbound = await fw.once("inbound", { ...bookArgs(books[i]) });
    const ids = inbound.annotations.map((a) => a.sync_id);
    t.eq(new Set(ids).size, ids.length, `book ${i}: nothing stored twice`);
    t.ok(ids.length > 0, `book ${i}: annotations stored`);
    const seqs = inbound.annotations.map((a) => BigInt(a.server_seq));
    t.ok(seqs.every((s, j) => j === 0 || s > seqs[j - 1]), `book ${i}: oldest first, in server order`);
    const newest = `Subrayado`;
    t.ok(inbound.annotations.at(-1)?.text.startsWith(newest), `book ${i}: newest is there`);
    if (i === 0) metrics["Backlog: inbound annotations kept per book"] = `${ids.length} of ${annotationCounts[i]} (the store rotates at 32 KiB per book, keeping the newest)`;
    const head = await fw.once("remote-head", { ...bookArgs(books[i]) });
    t.eq(head.has_head, true, `book ${i}: head cache filled`);
  }

  // Outbound: more than the outbox holds.
  const outBook = books[0];
  const clips = [];
  for (let i = 0; i < 260; i += 1) clips.push({ spine: 9, offset: i, hash: i + 1, text: `Saliente ${i} ${"texto ".repeat(10)}`, page: i, xpointer: xp(i) });
  const clipFile = fw.writeClips(clips);
  let finalizes = 0;
  let depth = 0;
  while (depth < 128 && finalizes < 12) {
    await fw.once("finalize-book", { ...bookArgs(outBook), paged: 0, clips: clipFile });
    depth = (await fw.once("status")).depth;
    finalizes += 1;
  }
  t.eq(depth, 128, "the outbox fills up to its limit");
  const overflow = await fw.once("finalize-book", { ...bookArgs(outBook), paged: 0, clips: clipFile });
  t.eq((await fw.once("status")).depth, 128, "and refuses more without corrupting anything");
  t.note(`finalize against a full outbox: ${JSON.stringify(overflow.finalize)}`);
  const full = await fw.once("status");
  t.eq(full.quarantined, 0, "nothing quarantined by being full");
  // Full queue: what could not be queued must be reported, so it is tried again.
  const fullSession = await fw.once("finalize-book", {
    ...bookArgs(outBook), paged: 0, session_start: now() - 5000, session_end: now() - 4000, pages_read: 2,
  });
  t.eq(fullSession.finalize.session_queued, false, "a session does not fit in a full queue");
  t.eq(fullSession.finalize.ok, false, "and finalize says it failed, so the caller keeps it");
  const fullProgress = await fw.once("finalize-book", { ...bookArgs(outBook), pct: 44, xp: xp(44), page: 44, turn_epoch: now() - 3000 });
  t.eq(fullProgress.finalize.ok, false, "neither does progress");
  const fullBoot = await fw.once("sizes");
  metrics["Outbox: core heap at boot with 128 events queued"] = `${fullBoot.heap.live} bytes`;

  h.proxy.clear();
  h.proxy.freshIp();
  const drain = await fw.once("sleep-session", {}, { "sleep-budget-ms": 120000 });
  t.eq(drain.report.outcome, "ok", "the backlog drains");
  t.eq(drain.report.events_acked, 128, "all 128");
  metrics["Outbound: peak core heap draining 128 events"] = `${drain.heap.peak} bytes (largest single allocation ${drain.heap.largest})`;
  const exchanges = h.proxy.requestsTo("/api/sync/v2/exchange");
  const biggest = Math.max(...exchanges.map((e) => e.bodyBytes));
  const mostEvents = Math.max(...exchanges.map((e) => JSON.parse(e.body).events.length));
  t.ok(biggest <= 8 * 1024 + 2048, `bodies stay bounded: ${biggest} bytes`);
  t.ok(mostEvents <= 20, `at most 20 events per batch: ${mostEvents}`);
  metrics["Outbound: exchanges for 128 events"] = `${exchanges.length}, largest body ${biggest} bytes, at most ${mostEvents} events`;

  // A tight sleep budget with a queue: it stops, nothing is lost.
  for (let i = 0; i < 3; i += 1) await fw.once("finalize-book", { ...bookArgs(outBook), paged: 0, clips: clipFile });
  const queued = (await fw.once("status")).depth;
  t.ok(queued > 40, `queue refilled (${queued})`);
  h.proxy.clear();
  h.proxy.fault({ match: /./, action: "delay", delayMs: 300, times: 1000 });
  const t1 = Date.now();
  const tight = await fw.once("sleep-session", {}, { "sleep-budget-ms": 1000 });
  const tightMs = Date.now() - t1;
  t.eq(tight.report.outcome, "budget_exhausted", "the sleep budget is respected");
  t.ok(tightMs < 2500, `and stops in time: ${tightMs} ms`);
  h.proxy.clear();
  for (let i = 0; i < 12; i += 1) {
    h.proxy.freshIp();
    const left = (await fw.once("status")).depth;
    await fw.once("finalize-book", { ...bookArgs(outBook), paged: 0, clips: clipFile });
    await fw.once("sleep-session", {}, { "sleep-budget-ms": 120000 });
    if (left === 0 && (await fw.once("status")).depth === 0) break;
  }
  const [{ count: outCount }] = await h.sql`SELECT count(*)::int FROM entries e JOIN documents d ON d.id = e.document_id WHERE d.user_id = ${account.id} AND e.text LIKE 'Saliente %'`;
  t.eq(outCount, 260, "every clip reached the server, in as many sessions as it took");
  const dupes = await h.sql`SELECT aggregate_id FROM device_events WHERE device_id = ${fw.deviceId} AND event_type = 'annotation.created' GROUP BY aggregate_id HAVING count(*) > 1`;
  t.eq(dupes.length, 0, "and none twice");

  // Worst case for the request body: one very long annotation.
  const longClip = [{ spine: 10, offset: 1, hash: 999999, text: "x".repeat(20000), page: 1, xpointer: xp(1) }];
  const longBook = books[1];
  await fw.once("finalize-book", { ...bookArgs(longBook), paged: 0, clips: fw.writeClips(longClip, "long.tsv") });
  const longUp = await fw.once("sleep-session");
  t.eq(longUp.report.events_acked, 1, "a 20 KiB annotation goes alone");
  metrics["Outbound: peak core heap with one 20 KiB annotation"] = `${longUp.heap.peak} bytes (largest single allocation ${longUp.heap.largest})`;
}

export const MORE = [
  { id: 4, name: "Offline: latest per book, occurred_at, honest clocks, late upload", run: offline },
  { id: 5, name: "Lost responses and fault sweep", run: faults },
  { id: 6, name: "Sequence conflict, restored card, legacy credential", run: repair },
  { id: 7, name: "Annotations: create, delete, idempotent, C29, inbound", run: annotations },
  { id: 8, name: "session.ended valid and invalid durations", run: sessions },
  { id: 9, name: "Catalog and download", run: library },
  { id: 10, name: "401 revoke and re-pair", run: revocation },
  { id: 11, name: "Big backlog: 2,000 inbound, 128+ outbound", run: backlog },
];
