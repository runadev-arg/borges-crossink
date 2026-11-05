// What round 2 of the firmware review found. Each scenario hits the real server, and checks
// the rows the rest of the product reads: not only that the request was accepted.

import fs from "node:fs";
import path from "node:path";
import { book, bookArgs, bookInfo, eventsOf, iso, now, xp } from "./scenarios.mjs";
import { KoreaderSim, buildItem, comparableText, normalizedText } from "./koreader.mjs";

const onCard = (dir) => fs.readdirSync(dir, { withFileTypes: true }).flatMap((e) => (e.isDirectory() ? onCard(path.join(dir, e.name)) : [path.join(dir, e.name)]));
const cardHolds = (fw, needle) => onCard(fw.sd).filter((f) => fs.readFileSync(f).includes(needle));

// Pairs fw again, as a person does after Disconnect. rewrite, if given, edits the claim answer.
async function repair(h, fw, account, rewrite) {
  await fw.once("forget-credential");
  if (rewrite) h.proxy.fault({ match: "/api/devices/pairing/claim", action: "rewrite", rewrite });
  await h.pairFirmware(fw, account);
}

// ---------------------------------------------------------------------------------
// A1: another account on re-pair
// ---------------------------------------------------------------------------------

async function accountSwitch(h, t) {
  const first = await h.account("uno");
  const second = await h.account("dos");

  const fw = h.firmware();
  await h.pairFirmware(fw, first);
  const b = book(fw, { title: "Lo del primero" });
  const queueOne = async () => {
    const finalized = await fw.once("finalize-book", { ...bookArgs(b), pct: 40, xp: xp(40), page: 40, clips: fw.writeClips([{ spine: 1, offset: 1, hash: 1, text: "Subrayado del primero", xpointer: xp(40), page: 40 }]) });
    t.eq(finalized.finalize.progress_queued, true, "queued offline");
  };
  await queueOne();
  t.eq((await fw.once("status")).depth, 2, "progress and annotation waiting");

  // Same account: its queue goes up.
  await repair(h, fw, first);
  t.eq((await fw.once("status")).depth, 2, "re-pairing the same account keeps the queue");
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "and it syncs");
  t.eq((await eventsOf(h, fw.deviceId)).length, 2, "under its owner");

  // Another account: nothing of the first goes up under it.
  await fw.once("finalize-book", { ...bookArgs(b), pct: 55, xp: xp(55), page: 55 });
  const firstCursor = (await fw.once("status")).cursor;
  t.ok(BigInt(firstCursor) > 0n, "the first account had a cursor");
  await repair(h, fw, second);
  const after = await fw.once("status");
  t.eq(after.depth, 0, "another account empties the queue");
  t.eq(after.account_id, second.id, "and it is the new account's");
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "the new account syncs");
  const [{ count: leaked }] = await h.sql`SELECT count(*)::int FROM device_events WHERE user_id = ${second.id}`;
  t.eq(leaked, 0, "not one event of the first account under the second");
  const [{ count: docs }] = await h.sql`SELECT count(*)::int FROM documents WHERE user_id = ${second.id}`;
  t.eq(docs, 0, "not one of its books either");

  // Without an id, the username decides. fix-server may add or drop account.id: both work.
  const noId = (body) => {
    delete body.account.id;
    return body;
  };
  const named = h.firmware();
  await repair(h, named, first, noId);
  const nb = book(named, { title: "Por nombre" });
  await named.once("finalize-book", { ...bookArgs(nb), pct: 10, xp: xp(10), page: 10 });
  await repair(h, named, first, noId);
  t.eq((await named.once("status")).depth, 1, "same username, no id: the queue stays");
  await repair(h, named, second, noId);
  t.eq((await named.once("status")).depth, 0, "another username, no id: the queue goes");
  t.eq((await named.once("manual-session")).report.outcome, "ok", "and it syncs");
  const [{ count: leakedByName }] = await h.sql`SELECT count(*)::int FROM device_events WHERE user_id = ${second.id} AND device_id = ${named.deviceId}`;
  t.eq(leakedByName, 0, "nothing leaked when the identity was only a username");

  // The server starting to send an id must not look like somebody else.
  const upgraded = h.firmware();
  await repair(h, upgraded, first, noId);
  const ub = book(upgraded, { title: "Con id después" });
  await upgraded.once("finalize-book", { ...bookArgs(ub), pct: 11, xp: xp(11), page: 11 });
  await repair(h, upgraded, first);
  t.eq((await upgraded.once("status")).depth, 1, "the same username now with an id: the queue stays");
}

// ---------------------------------------------------------------------------------
// A2, A3, A4: the token at rest, Disconnect, the old credential
// ---------------------------------------------------------------------------------

async function tokenAndDisconnect(h, t) {
  const account = await h.account("clave");
  const fw = h.firmware();
  h.proxy.clear();
  await h.pairFirmware(fw, account);
  const claim = h.proxy.requestsTo("/api/devices/pairing/claim")[0];
  const token = JSON.parse(claim.responseBody).credential.token;
  t.ok(token.length > 10, "the proxy saw the token go by");

  // A2: nowhere on the card, not even the response it came in.
  t.eq(cardHolds(fw, token).map((f) => path.relative(fw.sd, f)), [], "the token is on no file of the card");
  t.ok(!fs.existsSync(path.join(fw.borgesDir(), "spool", "resp.json")), "the claim's answer does not stay in the spool");
  const mac = Buffer.from(fw.mac, "hex");
  const xored = Buffer.from(token, "ascii").map((byte, i) => byte ^ mac[i % 6]);
  t.eq(cardHolds(fw, Buffer.from(xored)).length, 0, "nor XORed with the MAC, which is printed on the box");

  // Moved to another chip, the card proves nothing.
  const stolen = await fw.once("status", {}, { "nvs-salt": "another-chip" });
  t.eq(stolen.paired, false, "the card on another chip has no token");
  t.eq(stolen.account_id, account.id, "and still knows whose queue it holds");
  t.eq((await fw.once("status")).paired, true, "back on its own chip it is paired");
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "and works");

  // A3 online: the token dies now, and what replaced it is known to nobody.
  const works = async (bearer) => (await fetch(`${h.proxy.url}/api/sync/v2/capabilities`, { headers: { Authorization: `Bearer ${bearer}` } })).status;
  t.eq(await works(token), 200, "the token works before");
  h.proxy.clear();
  const gone = await fw.once("disconnect", { online: 1 });
  t.eq(gone.result, "server_invalidated", "online: the server confirmed it");
  t.eq((await fw.once("status")).paired, false, "unpaired");
  t.eq(await works(token), 401, "the old token is refused at once");
  const rotation = h.proxy.requestsTo("/api/devices/self/credentials/rotate")[0];
  const thrown = JSON.parse(rotation.responseBody).credential;
  t.eq(cardHolds(fw, thrown.token).length, 0, "the rotated token was not kept");
  t.ok(new Date(thrown.expires_at).getTime() <= Date.now() + 61000, "and expires within the minute");
  const [{ count: live }] = await h.sql`SELECT count(*)::int FROM device_credentials WHERE device_id = ${fw.deviceId} AND revoked_at IS NULL AND (expires_at IS NULL OR expires_at > now() + interval '2 minutes')`;
  t.eq(live, 0, "no credential of this device outlives the minute");

  // A3 offline: said out loud, not hidden.
  const offline = h.firmware();
  await h.pairFirmware(offline, account);
  const before = await offline.once("status");
  t.eq(before.paired, true, "paired");
  const down = await offline.once("disconnect", { online: 0 });
  t.eq(down.result, "still_valid_on_server", "offline: the screen must say the server still has it");

  // A3 with the server unreachable, same thing.
  const lost = h.firmware();
  await h.pairFirmware(lost, account);
  h.proxy.fault({ match: "/api/devices/self/credentials/rotate", action: "drop-before", times: 2 });
  t.eq((await lost.once("disconnect", { online: 1 })).result, "still_valid_on_server", "no answer: same");

  // A4: once, and never after a Disconnect.
  const importer = h.firmware();
  const kr = new KoreaderSim({ baseUrl: h.proxy.url, kobo: h.kobo, name: "Viejo", platform: "crossink-x4", externalId: `x4-legacy-${importer.name}` });
  await h.approve(account, (await kr.pairingRequest()).user_code);
  const claimed = await kr.pairingClaim();
  const legacyDir = path.join(importer.sd, ".borges");
  fs.mkdirSync(legacyDir, { recursive: true });
  const imac = Buffer.from(importer.mac, "hex");
  const obf = Buffer.from(kr.token, "ascii").map((byte, i) => byte ^ imac[i % 6]);
  fs.writeFileSync(path.join(legacyDir, "borges.json"), JSON.stringify({ baseUrl: "https://contract.invalid/", deviceId: claimed.device.id, token_obf: Buffer.from(obf).toString("base64") }));
  const imported = await importer.once("status");
  t.eq(imported.paired, true, "the old credential is imported");
  t.ok(imported.base_url.startsWith("http://127.0.0.1"), "its base URL means nothing");
  // The row was paired by the old firmware: the first session must say what runs now, even
  // with nothing to send, so only the pull can carry it.
  t.eq((await importer.once("manual-session")).report.outcome, "ok", "the imported credential syncs");
  const [row] = await h.sql`SELECT client_version, firmware_version FROM devices WHERE id = ${claimed.device.id}`;
  t.eq(row.client_version, "crossink-borges/1.6.1-contract", "the device row says what runs now");
  t.eq(row.firmware_version, "1.6.1-contract", "and its firmware");
  t.eq((await importer.once("disconnect", { online: 0 })).result, "still_valid_on_server", "disconnected");
  t.eq((await importer.once("status")).paired, false, "and the next boot does not bring it back");
  t.ok(fs.existsSync(path.join(legacyDir, "borges.json")), "the old file stays, for a rollback");
}

// ---------------------------------------------------------------------------------
// A11: page time, in the tables the dashboard reads
// ---------------------------------------------------------------------------------

async function pageStats(h, t) {
  const { account, fw, kr } = await h.fwAndKoreader("panel");
  const b = book(fw, { title: "Sin subrayados", author: "Silvina Ocampo" });
  const start = now() - 3600;
  const rows = [];
  for (let i = 0; i < 40; i += 1) rows.push(`${i + 1}:${start + i * 60}:50`);
  rows.push(`41:${start + 40 * 60}:3`); // Flipped past.
  const queued = await fw.once("finalize-book", { ...bookArgs(b), pct: 41, xp: xp(41), page: 41, page_stats: rows.join(",") });
  t.eq(queued.finalize.page_stat_events, 2, "32 + 8 rows");
  t.eq(queued.finalize.page_stats_left, 0, "out of the buffer");
  const up = await fw.once("sleep-session");
  t.eq(up.report.outcome, "ok", "accepted");
  t.eq(up.report.events_rejected, 0, "none rejected");

  const stored = await h.sql`SELECT page, duration, total_pages, start_time FROM page_stats WHERE user_id = ${account.id} AND book_md5 = ${b.md5} ORDER BY start_time`;
  t.eq(stored.length, 40, "every page in page_stats, under this account");
  t.eq(stored[0].page, 1, "page");
  t.eq(stored[0].duration, 50, "duration");
  t.eq(stored[0].total_pages, b.total, "total pages");
  t.eq(new Date(stored[0].start_time).toISOString(), iso(start), "start time");

  const [doc] = await h.sql`SELECT title, author, book_hash FROM documents WHERE user_id = ${account.id} AND book_hash = ${b.md5}`;
  t.eq(doc?.title, b.title, "a book with no highlights gets its document");
  t.eq(doc?.author, b.author, "with its author");

  const [{ data }] = await h.sql`SELECT get_account_dashboard_data(${account.id}::uuid, '1M') AS data`;
  const dashboard = typeof data === "string" ? JSON.parse(data) : data;
  const text = JSON.stringify(dashboard);
  t.ok(text.includes(b.md5) || text.includes(b.title), "the dashboard knows the book");
  t.near(Number(dashboard.time_totals.all_time_minutes), (40 * 50) / 60, 0.2, "and adds up its time");
  t.ok(dashboard.streaks.total_reading_days >= 1, `and counts the day: ${JSON.stringify(dashboard.streaks)}`);
  t.ok(dashboard.heatmap_days.some((d) => Number(d.minutes ?? d.value ?? 0) > 0), `and the heatmap lights up: ${JSON.stringify(dashboard.heatmap_days.slice(-2))}`);

  // The same rows again, as a retry would: unique on the server, so nothing doubles.
  await fw.once("finalize-book", { ...bookArgs(b), paged: 0, page_stats: rows.join(",") });
  await fw.once("sleep-session");
  const [{ count: again }] = await h.sql`SELECT count(*)::int FROM page_stats WHERE user_id = ${account.id} AND book_md5 = ${b.md5}`;
  t.eq(again, 40, "sent twice, counted once");

  // A clock nobody vouches for has nothing honest to report.
  const dark = book(fw, { title: "A oscuras" });
  const unclocked = await fw.once("finalize-book", { ...bookArgs(dark), paged: 0, turn_precision: "approximate", page_stats: `1:${start}:60` });
  t.eq(unclocked.finalize.page_stat_events, 0, "no page time from an approximate clock");

  // KOReader's sync still sees the event, as any other.
  const pulled = await kr.syncAll();
  t.ok(pulled.events.some((e) => e.event_type === "page_stat.recorded"), "other devices see it pass");
}

// ---------------------------------------------------------------------------------
// A13: an annotation that arrives usable in KOReader
// ---------------------------------------------------------------------------------

async function annotationsCross(h, t) {
  const { account, fw, kr } = await h.fwAndKoreader("cruce");
  const b = book(fw, { title: "Cruce" });

  // Line ends as the card may have them, and what a typesetter leaves in a passage.
  const stored = "Primera línea\r\nsegunda línea con nbsp y gui­ón";
  const start = xp(10).replace(".0", ".4");
  const end = xp(11).replace(".0", ".20");
  const clips = [
    { spine: 3, offset: 10, hash: 1, text: stored, chapter: "Uno", xpointer: start, xpointerEnd: end, pctMicro: 25000000 },
    { spine: 3, offset: 12, hash: 2, bookmark: true, text: "", xpointer: xp(12), pctMicro: 50000000 },
    { spine: 3, offset: 13, hash: 3, text: "Sin final", xpointer: xp(13), pctMicro: 60000000 },
  ];
  await fw.once("finalize-book", { ...bookArgs(b), paged: 0, clips: fw.writeClips(clips) });
  const sent = await fw.once("sleep-session");
  t.eq(sent.report.events_rejected, 0, "the server accepts all three");

  const pull = await kr.syncAll();
  const fromFw = pull.events.filter((e) => e.origin_device.id === fw.deviceId && e.aggregate_type === "annotation");
  t.eq(fromFw.length, 3, "KOReader receives the three");
  const highlight = fromFw.find((e) => e.event_type === "annotation.created" && e.payload.position);
  const mark = fromFw.find((e) => e.event_type === "bookmark.upserted");
  const noEnd = fromFw.find((e) => e.event_type === "annotation.created" && !e.payload.position);

  // annotationadapter.lua:buildItem, with the document answering as the book would.
  const inDocument = (x) => typeof x === "string" && /^\/body\//.test(x);
  const item = buildItem(highlight.payload, "rolling", inDocument, highlight.directive_metadata.annotation_revision);
  t.ok(item.item, `the highlight passes KOReader's validation: ${item.error}`);
  t.eq(item.item?.page, start, "page is the start xpointer");
  t.eq(item.item?.pos0, start, "pos0");
  t.eq(item.item?.pos1, end, "pos1");
  t.eq(item.item?.pageno, 25, "the book-wide page, from the percent");
  t.eq(item.item?.drawer, "lighten", "drawer");
  t.eq(item.item?.text, "Primera línea\nsegunda línea con nbsp y gui­ón", "text normalized as normalizedText does");
  t.ok(!highlight.payload.text.includes("\r"), "no carriage return on the wire");
  // applyRemote then reads the range and compares. The device's text must survive that.
  const whatTheDocumentSays = "Primera línea segunda línea con nbsp y guión";
  t.eq(comparableText(highlight.payload.text), comparableText(whatTheDocumentSays), "the text matches the range as KOReader compares it");
  t.eq(normalizedText("a\r\nb\rc"), "a\nb\nc", "(the port of normalizedText)");

  const bookmark = buildItem(mark.payload, "rolling", inDocument, mark.directive_metadata.annotation_revision);
  t.ok(bookmark.item, `the bookmark passes: ${bookmark.error}`);
  t.eq(bookmark.item?.pageno, 50, "a bookmark has a page");
  t.eq(mark.payload.text, "Bookmark - page 50", "and a text, as annotationText writes it");
  t.eq(bookmark.item?.drawer, undefined, "and no drawer: that is what makes it a bookmark");

  // Why pos1 is not optional: this is what KOReader does without it.
  t.eq(buildItem(noEnd.payload, "rolling", inDocument, "1").error, "invalid_rolling_locator", "without an end, KOReader cannot place a highlight");

  // Stored as the web and exports read it.
  const [doc] = await h.sql`SELECT id FROM documents WHERE user_id = ${account.id} AND book_hash = ${b.md5}`;
  const [entry] = await h.sql`SELECT id, page, text, xpointer, sort FROM entries WHERE document_id = ${doc.id} AND xpointer = ${start}`;
  t.eq(entry?.page, 25, "the entry has the book-wide page");
  t.eq(JSON.parse(entry?.sort ?? "{}").pos1, end, "and both ends");

  // A web edit, then a delete here: a delete, not a conflict.
  const edit = await h.web("PATCH", `/api/highlights/notes/${entry.id}`, { cookie: account.cookie, body: { note: "Nota desde la web" } });
  t.eq(edit.status, 200, `the web edits the note: ${JSON.stringify(edit.body)}`);
  const revision = String(edit.body.annotation_revision);
  t.ok(BigInt(revision) > 1n, `and that made a revision: ${revision}`);
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "the firmware pulls the edit");

  await fw.once("finalize-book", { ...bookArgs(b), paged: 0, clips: fw.writeClips(clips.slice(1)) });
  const queued = (await fw.once("outbox")).events.find((e) => e.envelope[0]?.event_type === "annotation.deleted" || e.envelope.event_type === "annotation.deleted");
  t.ok(queued, "the delete is queued");
  const envelope = Array.isArray(queued?.envelope) ? queued.envelope[0] : queued?.envelope;
  t.eq(envelope?.payload?.base_revision, revision, "built on the revision the web left");
  const deleted = await fw.once("sleep-session");
  t.eq(deleted.report.events_rejected, 0, "accepted");
  const [{ count: conflicts }] = await h.sql`SELECT count(*)::int FROM sync_conflicts WHERE user_id = ${account.id} AND status = 'open'`;
  t.eq(conflicts, 0, "no conflict for the person to resolve");
  const [{ count: alive }] = await h.sql`SELECT count(*)::int FROM entries WHERE document_id = ${doc.id} AND xpointer = ${start}`;
  t.eq(alive, 0, "and the entry is gone");
}

// ---------------------------------------------------------------------------------
// A14: a long reading, A15: the queue depth on pulls
// ---------------------------------------------------------------------------------

async function longSessions(h, t) {
  const { fw } = await h.fwAndKoreader("larga");
  const b = book(fw, { title: "Una noche entera" });
  const end = now() - 7200;
  const start = end - 9 * 3600;
  const done = await fw.once("finalize-book", { ...bookArgs(b), paged: 0, session_start: start, session_end: end, pages_read: 300, start_page: 10, end_page: 310 });
  t.eq(done.finalize.session_queued, true, "a nine hour reading is not dropped");
  const up = await fw.once("sleep-session");
  t.eq(up.report.events_rejected, 0, "every piece is accepted");
  const rows = await h.sql`SELECT duration_seconds, pages_read, start_page, end_page, started_at, ended_at FROM reading_sessions WHERE device_id = ${fw.deviceId} ORDER BY started_at`;
  t.eq(rows.map((r) => r.duration_seconds), [14400, 14400, 3600], "4 h + 4 h + 1 h in reading_sessions");
  t.eq(rows[0].start_page, 10, "the first has the start page");
  t.eq(rows.map((r) => r.pages_read), [0, 0, 300], "pages counted once");
  t.eq(rows[2].end_page, 310, "and the end page");
  t.eq(new Date(rows[0].ended_at).toISOString(), new Date(rows[1].started_at).toISOString(), "no gap between the pieces");

  // The fleet view reads queue_depth from what the last request said.
  const q = book(fw, { title: "Sin subir" });
  await fw.once("finalize-book", { ...bookArgs(q), pct: 5, xp: xp(5), page: 5 });
  await fw.once("book-opened", { ...bookArgs(q) });
  h.proxy.clear();
  const proc = fw.boot();
  try {
    await proc.cmd("manual-session");
  } finally {
    await proc.close();
  }
  const pulls = h.proxy.requestsTo("/api/sync/v2/pull");
  t.ok(pulls.length >= 1, "it pulled");
  t.eq(JSON.parse(pulls[0].body).client?.queue_depth, 1, "a pull says what is waiting");
}

export const ROUND2 = [
  { id: 12, name: "Another account on re-pair (A1)", run: accountSwitch },
  { id: 13, name: "Token at rest, Disconnect, import once (A2, A3, A4)", run: tokenAndDisconnect },
  { id: 14, name: "Page time in the dashboard's tables (A11)", run: pageStats },
  { id: 15, name: "Annotations that KOReader can place (A13)", run: annotationsCross },
  { id: 16, name: "Long sessions, queue depth (A14, A15)", run: longSessions },
];
