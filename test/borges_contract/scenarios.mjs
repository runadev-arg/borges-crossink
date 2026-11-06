// The contract. Every scenario hits the real server; checks look at what both clients
// answered and at the rows in the database.

import { buildEpub, koreaderPartialMd5 } from "./epub.mjs";
import { ApiFailure } from "./koreader.mjs";

export const now = () => Math.floor(Date.now() / 1000);

let bookCounter = 0;
export function book(fw, { title, author = "Jorge Luis Borges", padBytes = 0, fileName } = {}) {
  bookCounter += 1;
  const realTitle = title ?? `Ficciones ${bookCounter}`;
  const bytes = buildEpub({ title: realTitle, author, padBytes, seed: `seed-${bookCounter}` });
  const name = fileName ?? `libro-${bookCounter}.epub`;
  const sdPath = fw ? fw.putBook(name, bytes) : null;
  return { title: realTitle, author, bytes, md5: koreaderPartialMd5(bytes), path: sdPath, name, total: 100 };
}
export const bookArgs = (b) => ({ file: b.path, title: b.title, author: b.author, total: b.total });
export const bookInfo = (b) => ({ title: b.title, author: b.author, file: b.name, total_pages: b.total });
export const xp = (n) => `/body/DocFragment[1]/body/p[${n}]/text().0`;

export async function krRead(kr, b, page, { epoch, jump = false, xpointer } = {}) {
  kr.enqueueProgress({ book: b.md5, page, totalPages: b.total, xpointer: xpointer ?? xp(page), isJump: jump, title: b.title, author: b.author, epoch });
  return kr.syncAll();
}

export const eventsOf = (h, deviceId) => h.sql`
  SELECT client_event_id, client_sequence, event_type, aggregate_id, occurred_at, time_precision, payload,
         processing_status, rejection_code, server_sequence, processing_metadata
  FROM device_events WHERE device_id = ${deviceId} ORDER BY server_sequence`;

export const headOf = (h, md5, userId) => h.sql`
  SELECT ps.device_id, ps.percentage, ps.xpointer, ps.explicit_jump, ps.occurred_at, ps.time_precision, ps.revision, d.name
  FROM device_progress_state ps JOIN devices d ON d.id = ps.device_id
  WHERE ps.book_key = ${`koreader_partial_md5:${md5}`} AND ps.user_id = ${userId}
  ORDER BY CASE WHEN ps.time_precision IN ('exact', 'anchored') THEN ps.occurred_at ELSE ps.received_at END DESC, ps.revision DESC`;

export const iso = (epoch) => new Date(epoch * 1000).toISOString();

// ---------------------------------------------------------------------------------
// 1. Pairing
// ---------------------------------------------------------------------------------

async function pairing(h, t) {
  const account = await h.account("pareo");

  // Happy path, polling before the approval too.
  const fw = h.firmware();
  let proc = fw.boot();
  const started = await proc.cmd("pair-start");
  t.eq(started.state, "waiting", "pair-start waits for approval");
  t.ok(/^[A-Z0-9]{4}-[A-Z0-9]{4}$/.test(started.user_code), `user code shape: ${started.user_code}`);
  t.ok(started.qr_url.startsWith(`${h.proxy.url}/devices/pair?code=${started.user_code}`), `qr url: ${started.qr_url}`);
  const [pending] = await h.sql`SELECT status, platform, external_id, user_id FROM device_pairing_requests WHERE device_name = 'Xteink X4' ORDER BY created_at DESC LIMIT 1`;
  t.eq(pending.status, "pending", "request row is pending");
  t.eq(pending.platform, "crossink-x4", "request row platform");
  t.eq(pending.user_id, null, "a code has no owner until it is approved");
  const early = await proc.cmd("pair-poll");
  t.eq(early.state, "waiting", "poll before the approval keeps waiting");
  await h.approve(account, started.user_code);
  const done = await proc.cmd("pair-poll");
  t.eq(done.state, "done", "poll after the approval claims");
  await proc.close();

  const status = await fw.once("status");
  t.eq(status.paired, true, "credential survives a reboot");
  t.ok(!/toto_|"token":/.test(JSON.stringify(status)), "no token in any output");
  const [device] = await h.sql`SELECT id, platform, external_id, user_id, name, status, firmware_version, client_version FROM devices WHERE id = ${status.device_id}`;
  t.eq(device.platform, "crossink-x4", "device platform");
  t.eq(device.external_id, status.external_id, "device external id");
  t.ok(device.external_id.startsWith("x4-"), "external id is prefixed and hashed");
  t.eq(device.user_id, account.id, "device belongs to the approving account");
  t.eq(status.account_id, account.id, "the device knows its account");
  const manual = await fw.once("manual-session");
  t.eq(manual.report.outcome, "ok", "the issued credential works for sync");
  const catalog = await fw.once("catalog", { page: 1, limit: 5 });
  t.eq(catalog.result, "ok", "the issued credential works for the library");
  const [{ count: active }] = await h.sql`SELECT count(*)::int FROM device_credentials WHERE device_id = ${device.id} AND revoked_at IS NULL`;
  t.eq(active, 1, "one active credential");

  // Reject.
  const rejected = h.firmware();
  proc = rejected.boot();
  const r1 = await proc.cmd("pair-start");
  const rej = await h.reject(account, r1.user_code);
  t.eq(rej.status, 200, "reject accepted");
  const r2 = await proc.cmd("pair-poll");
  t.eq(r2.state, "rejected", "a rejected code ends the flow");
  t.eq(r2.paired, false, "no credential after a rejection");
  await proc.close();
  const [{ count: rejectedDevices }] = await h.sql`SELECT count(*)::int FROM devices WHERE external_id = ${(await rejected.once("status")).external_id}`;
  t.eq(rejectedDevices, 0, "a rejection creates no device");

  // Expiry on the server: the row expires, the device finds out by asking.
  const expired = h.firmware();
  proc = expired.boot();
  const e1 = await proc.cmd("pair-start");
  await h.sql`UPDATE device_pairing_requests SET created_at = now() - interval '10 minutes', expires_at = now() - interval '1 minute' WHERE status = 'pending'`;
  const e2 = await proc.cmd("pair-poll");
  t.eq(e2.state, "expired", `expired on the server (${e1.user_code})`);
  await proc.close();

  // Expiry on the device: past expires_in it does not even ask.
  const local = h.firmware();
  proc = local.boot();
  await proc.cmd("pair-start");
  h.proxy.clear();
  const l2 = await proc.cmd("pair-poll", { bump_ms: 301000 });
  t.eq(l2.state, "expired", "expired locally");
  t.eq(h.proxy.requestsTo("/api/devices/pairing/status").length, 0, "local expiry costs no request");
  const l3 = await proc.cmd("pair-start");
  t.eq(l3.state, "waiting", "start after expiry gets a new code");
  await proc.close();

  // Claim twice, on the wire.
  const kr = await h.koreader(account);
  let second = null;
  try {
    await kr.pairingClaim();
  } catch (error) {
    second = error;
  }
  t.ok(second instanceof ApiFailure, "a second claim fails");
  t.eq(second?.code, "pairing_already_claimed", "second claim code");
  const [{ count: krActive }] = await h.sql`SELECT count(*)::int FROM device_credentials WHERE device_id = ${kr.deviceId} AND revoked_at IS NULL`;
  t.eq(krActive, 1, "a second claim issues nothing");

  // Claim whose answer is lost: the retry finds it claimed. A new code recovers it.
  const lost = h.firmware();
  proc = lost.boot();
  const c1 = await proc.cmd("pair-start");
  await h.approve(account, c1.user_code);
  h.proxy.fault({ match: "/api/devices/pairing/claim", action: "drop-after" });
  // Not repeated in the same breath, since the server claims once: it keeps waiting, and the next
  // poll is told it was claimed.
  let c2 = await proc.cmd("pair-poll");
  if (c2.state === "waiting") c2 = await proc.cmd("pair-poll");
  t.eq(c2.state, "claim_lost", "lost claim answer is reported as such");
  t.eq(c2.paired, false, "no credential saved");
  const c3 = await proc.cmd("pair-start");
  t.eq(c3.state, "waiting", "a new code after a lost claim");
  await h.approve(account, c3.user_code);
  const c4 = await proc.cmd("pair-poll");
  t.eq(c4.state, "done", "second attempt pairs");
  await proc.close();
  const lostExternal = (await lost.once("status")).external_id;
  const lostDevices = await h.sql`SELECT id FROM devices WHERE external_id = ${lostExternal} AND user_id = ${account.id}`;
  t.eq(lostDevices.length, 1, "same device row after a lost claim");
  t.eq(lostDevices[0].id, c4.device_id, "the claim belongs to that row");

  // Same external_id, paired again: same row.
  const before = fw.deviceId ?? status.device_id;
  await fw.once("forget-credential");
  t.eq((await fw.once("status")).paired, false, "credential forgotten");
  await h.pairFirmware(fw, account);
  const again = await fw.once("status");
  t.eq(again.device_id, before, "re-pair yields the same device id");
  const [{ count: rows }] = await h.sql`SELECT count(*)::int FROM devices WHERE external_id = ${again.external_id}`;
  t.eq(rows, 1, "still one device row");
  const creds = await h.sql`SELECT revoked_at FROM device_credentials WHERE device_id = ${before}`;
  t.note(`after re-pair the device has ${creds.length} credentials, ${creds.filter((c) => !c.revoked_at).length} active`);
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "the new credential works");
}

// ---------------------------------------------------------------------------------
// 2. "Where do you want to keep reading?"
// ---------------------------------------------------------------------------------

async function resume(h, t) {
  const { account, fw, kr } = await h.fwAndKoreader("resume");
  const b = book(fw);
  t.eq((await fw.once("md5", { file: b.path })).md5, b.md5, "core and an independent partial MD5 agree");

  // KOReader reads to 60%.
  const readAt = now() - 300;
  await krRead(kr, b, 60, { epoch: readAt });
  h.proxy.clear();
  let proc = fw.boot();
  let open = await proc.cmd("open-session", { ...bookArgs(b), pct: 10, xp: xp(10), page: 10 });
  t.eq(open.report.outcome, "awaiting_choice", "open asks");
  t.eq(open.decision.ask, true, "decision asks");
  t.near(open.decision.remote_pct, 60, 0.0001, "prompt shows that position");
  t.eq(open.decision.remote_xpointer, xp(60), "prompt carries the xpointer");
  t.eq(open.decision.xpointer_usable, true, "xpointer is usable");
  t.eq(open.decision.remote_device, kr.name, "prompt names the device");
  t.eq(open.decision.occurred_at, iso(readAt).replace(".000Z", ".000Z"), "date of the reading");
  t.eq(open.decision.warnings, 0, "no warnings going forward");
  t.eq(h.proxy.requestsTo("/api/sync/v2/exchange").length, 0, "nothing goes up while the prompt is open");

  // Go.
  const go = await proc.cmd("apply-choice", { choice: "go", total: b.total, after_page: 60 });
  t.eq(go.report.outcome, "ok", "go syncs");
  t.eq(go.report.events_acked, 1, "own progress acked");
  await proc.close();
  const events = await eventsOf(h, fw.deviceId);
  t.eq(events.length, 1, "one firmware event");
  t.eq(events[0].event_type, "progress.changed", "event type");
  t.eq(events[0].payload.explicit_jump, true, "go is an explicit jump");
  t.near(events[0].payload.percentage, 60, 0.0001, "firmware moved to 60");
  t.eq(events[0].payload.xpointer, xp(60), "firmware moved to that xpointer");
  const head = await headOf(h, b.md5, account.id);
  t.eq(head[0].device_id, fw.deviceId, "firmware is the head now");
  t.eq(head[0].explicit_jump, true, "the state row says it was a jump");
  const undo = await fw.once("undo", { ...bookArgs(b), take: 1 });
  t.eq(undo.has_undo, true, "an undo point exists");
  t.near(undo.position?.pct, 10, 0.0001, "undo goes back to where it was");
  t.eq((await fw.once("undo", { ...bookArgs(b) })).has_undo, false, "undo is single use");

  // KOReader's next pull: its own event is `own`, the firmware's is about the same spot.
  const pulled = await kr.pull();
  const own = pulled.events.find((e) => e.origin_device.id === kr.deviceId);
  const other = pulled.events.find((e) => e.origin_device.id === fw.deviceId);
  t.ok(other, "KOReader receives the firmware's event");
  t.note(`KOReader directives after Go: own event=${own?.directive ?? "(already past)"}, firmware event=${other?.directive}`);
  if (own) t.eq(own.directive, "own", "own event is own");
  t.ok(["suggest_jump", "suggest_resume", "equivalent"].includes(other?.directive), `firmware jump directive is a known one: ${other?.directive}`);
  t.eq(other?.origin_device?.platform, "crossink-x4", "origin platform");
  // The same xpointer, so the reader that was there already has nothing to move to.
  t.eq(other?.directive, "equivalent", "same anchor is equivalent");
  const krHead = kr.decideHead(await kr.pull({ focusBook: b.md5, focusOnly: true }), b.md5);
  t.eq(krHead.valid, true, "KOReader understands the head");
  t.eq(krHead.event?.origin_device?.name, "Xteink X4", "KOReader names the firmware");
  t.eq(krHead.ask, true, "KOReader asks even at the same spot");

  // Stay path.
  await new Promise((r) => setTimeout(r, 1100));
  await krRead(kr, b, 70, { epoch: now() });
  await new Promise((r) => setTimeout(r, 1100));
  proc = fw.boot();
  open = await proc.cmd("open-session", { ...bookArgs(b), pct: 60, xp: xp(60), page: 60 });
  t.eq(open.decision.ask, true, "a new reading asks");
  t.near(open.decision.remote_pct, 70, 0.0001, "the new position");
  const stay = await proc.cmd("apply-choice", { choice: "stay", total: b.total });
  t.eq(stay.report.outcome, "ok", "stay syncs");
  await proc.close();
  const stayEvents = await eventsOf(h, fw.deviceId);
  const stayEvent = stayEvents[stayEvents.length - 1];
  t.eq(stayEvent.payload.explicit_jump, false, "stay is not a jump");
  t.near(stayEvent.payload.percentage, 60, 0.0001, "stay keeps the local position");
  t.near(new Date(stayEvent.occurred_at).getTime() / 1000, now(), 60, "stay stamps reading activity now");
  const stayHead = await headOf(h, b.md5, account.id);
  t.eq(stayHead[0].device_id, fw.deviceId, "after stay the firmware is the head");
  t.eq((await fw.once("undo", { ...bookArgs(b) })).has_undo, false, "stay leaves no undo");
  const krHead2 = kr.decideHead(await kr.pull({ focusBook: b.md5, focusOnly: true }), b.md5);
  t.eq(krHead2.ask, true, "KOReader is asked about the firmware's position");
  t.near(krHead2.event?.payload?.percentage, 60, 0.0001, "and offered 60");

  // Backwards warning on the firmware.
  await new Promise((r) => setTimeout(r, 2100));
  const krHeadBack = await krRead(kr, b, 20, { epoch: now() + 1 });
  t.eq(krHeadBack.pending, 0, "KOReader uploaded");
  proc = fw.boot();
  const back = await proc.cmd("open-session", { ...bookArgs(b), pct: 60, xp: xp(60), page: 60 });
  t.eq(back.decision.ask, true, "backwards still asks");
  t.eq(back.decision.warnings & 1, 1, "backwards warning");
  await proc.cmd("apply-choice", { choice: "stay", total: b.total });
  await proc.close();

  // Reverse: the firmware reads ahead, KOReader is the one who asks.
  await new Promise((r) => setTimeout(r, 2100));
  const turn = now();
  const finalize = await fw.once("finalize-book", { ...bookArgs(b), pct: 85, xp: xp(85), page: 85, turn_epoch: turn });
  t.eq(finalize.finalize.progress_queued, true, "finalize queues progress");
  const sleep = await fw.once("sleep-session");
  t.eq(sleep.report.outcome, "ok", "sleep session syncs");
  t.eq(sleep.report.events_acked, 1, "sleep drained the progress");
  const krCheck = await kr.pull({ focusBook: b.md5, focusOnly: true });
  const decided = kr.decideHead(krCheck, b.md5);
  t.eq(decided.ask, true, "KOReader focus_only asks");
  t.near(decided.event?.payload?.percentage, 85, 0.0001, "offered position");
  t.eq(decided.event?.payload?.xpointer, xp(85), "offered xpointer");
  t.eq(decided.event?.origin_device?.name, "Xteink X4", "offered device");
  t.eq(decided.event?.occurred_at, iso(turn), "occurred_at is the last page turn");
  t.eq(decided.event?.time_precision, "exact", "precision is what the clock said");
  t.eq(decided.event?.directive, "suggest_resume", "head directive");
  const cursorBefore = kr.cursor;
  t.eq(krCheck.events.length, 0, "focus_only delivers no history");
  t.eq(kr.cursor, cursorBefore, "focus_only does not move the cursor");
  const [fwRow] = await h.sql`SELECT firmware_version, client_version, protocol_version, queue_depth, last_sync_at FROM devices WHERE id = ${fw.deviceId}`;
  t.eq(fwRow.client_version, "crossink-borges/1.6.1-contract", "client version recorded");
  t.eq(fwRow.queue_depth, 0, "health reported an empty queue");
  const [krRow] = await h.sql`SELECT firmware_version, client_version FROM devices WHERE id = ${kr.deviceId}`;
  t.eq(krRow.client_version, kr.pluginVersion, "KOReader's version recorded");

  // The durable suggestion: the pull hands it over, and the choice settles it on the server.
  await new Promise((r) => setTimeout(r, 2100));
  await krRead(kr, b, 90, { epoch: now() + 1 });
  const openSuggestions = () => h.sql`
    SELECT id, status FROM sync_conflicts
    WHERE target_device_id = ${fw.deviceId} AND status = 'open' AND conflict_type IN ('progress_resume', 'progress_jump')`;
  await fw.once("manual-session");
  const suggested = await openSuggestions();
  t.ok(suggested.length >= 1, "the pull left an open suggestion for the firmware");
  h.proxy.clear();
  proc = fw.boot();
  const suggestedOpen = await proc.cmd("open-session", { ...bookArgs(b), pct: 85, xp: xp(85), page: 85 });
  t.eq(suggestedOpen.decision.ask, true, "the head asks");
  const accepted = await proc.cmd("apply-choice", { choice: "go", total: b.total, after_page: 90 });
  t.eq(accepted.report.outcome, "ok", "go syncs");
  await proc.close();
  const settled = h.proxy.requestsTo(/\/api\/sync\/v2\/suggestions\/[^/]+\/accept$/);
  t.eq(settled.length, 1, "go accepts the suggestion");
  t.eq(settled[0]?.status, 200, "and the server takes it");
  t.eq(settled[0]?.path.split("/")[5], suggested[suggested.length - 1]?.id, "the newest one");
}

// ---------------------------------------------------------------------------------
// 3. Who gets asked, and who never sees anything
// ---------------------------------------------------------------------------------

async function dedupe(h, t) {
  const { account, fw, kr } = await h.fwAndKoreader("dedupe");
  const b = book(fw);

  // No event at all.
  const first = await fw.once("open-session", { ...bookArgs(b), pct: 5, xp: xp(5), page: 5 });
  t.eq(first.decision.verdict, "no_event", "nobody read this edition");
  t.eq(first.report.outcome, "ok", "no prompt, session continues");

  // Answered with a clock behind KOReader's reading: the head stays KOReader's, and the
  // same event must not come back.
  await krRead(kr, b, 40, { epoch: now() - 60 });
  const behind = { "wall-offset": -3600 };
  let proc = fw.boot(behind);
  const ask = await proc.cmd("open-session", { ...bookArgs(b), pct: 5, xp: xp(5), page: 5 });
  t.eq(ask.decision.ask, true, "first time it asks");
  const stay = await proc.cmd("apply-choice", { choice: "stay", total: b.total });
  t.eq(stay.report.outcome, "ok", "choice synced");
  await proc.close();
  const head = await headOf(h, b.md5, account.id);
  t.eq(head[0].device_id, kr.deviceId, "the stay stamped an hour ago did not take the head");
  const again = await fw.once("open-session", { ...bookArgs(b), pct: 5, xp: xp(5), page: 5 }, behind);
  t.eq(again.decision.ask, false, "same event is not asked again");
  t.eq(again.decision.verdict, "already_chosen", "because it was answered");

  // This device read since: an answered event stays answered, as in resumeflow.lua.
  await fw.once("finalize-book", { ...bookArgs(b), pct: 6, xp: xp(6), page: 6, turn_epoch: now() - 3000 }, behind);
  await fw.once("sleep-session", {}, behind);
  t.eq((await headOf(h, b.md5, account.id))[0].device_id, kr.deviceId, "still KOReader's head");
  proc = fw.boot(behind);
  const readSince = await proc.cmd("open-session", { ...bookArgs(b), pct: 6, xp: xp(6), page: 6 });
  t.eq(readSince.decision.ask, false, "does not ask again after reading since");
  await proc.close();

  // A new event at the same spot asks again.
  await krRead(kr, b, 40, { epoch: now() - 30, xpointer: xp(40) });
  proc = fw.boot(behind);
  const samePlace = await proc.cmd("open-session", { ...bookArgs(b), pct: 6, xp: xp(6), page: 6 });
  t.eq(samePlace.decision.ask, true, "new event at the same spot asks again");
  t.eq(samePlace.decision.remote_xpointer, xp(40), "same spot");
  const fingerprintA = samePlace.decision.fingerprint;
  await proc.cmd("apply-choice", { choice: "go", total: b.total, after_page: 40 });
  await proc.close();

  // Own event: this device is the head, nothing to ask.
  await new Promise((r) => setTimeout(r, 1100));
  await fw.once("finalize-book", { ...bookArgs(b), pct: 41, xp: xp(41), page: 41, turn_epoch: now() });
  await fw.once("sleep-session");
  t.eq((await headOf(h, b.md5, account.id))[0].device_id, fw.deviceId, "firmware is the head");
  const own = await fw.once("open-session", { ...bookArgs(b), pct: 41, xp: xp(41), page: 41 });
  t.eq(own.decision.verdict, "own", "own event");
  t.eq(own.decision.ask, false, "own event does not ask");
  t.ok(fingerprintA.length > 0, "fingerprint exists");

  // Replayed progress in a pull never prompts; it only updates the head cache.
  await krRead(kr, b, 42, { epoch: now() + 2 });
  const manual = await fw.once("manual-session");
  t.eq(manual.report.outcome, "ok", "a manual session with no book cannot prompt");
  const cached = await fw.once("remote-head", { ...bookArgs(b) });
  t.eq(cached.has_head, true, "pull updated the head cache");
  t.near(cached.position?.pct, 42, 0.0001, "head cache position");
  t.eq(cached.device, kr.name, "head cache device");

  // Another account, same EPUB: nothing leaks either way.
  const other = await h.account("ajena");
  const otherFw = h.firmware();
  await h.pairFirmware(otherFw, other);
  const otherKr = await h.koreader(other, { name: "Kobo Ajeno" });
  otherFw.putBook(b.name, b.bytes);
  const isolated = await otherFw.once("open-session", { ...bookArgs(b), pct: 1, xp: xp(1), page: 1 });
  t.eq(isolated.decision.verdict, "no_event", "other account's firmware sees no event for the same EPUB");
  const otherPull = await otherKr.pull({ focusBook: b.md5, focusOnly: true });
  t.eq(otherPull.focusEvent, null, "other account's KOReader sees no focus event");
  const otherSync = await otherKr.syncAll();
  t.eq(otherSync.events.length, 0, "other account's pull is empty");
  const manualOther = await otherFw.once("manual-session");
  t.eq(manualOther.report.events_applied, 0, "other account's firmware applied nothing");
  t.eq((await otherFw.once("status")).cursor, "0", "other account's cursor stays at its own zero");
  t.eq((await otherFw.once("remote-head", { ...bookArgs(b) })).has_head, false, "no head cache leak");
  // And back: the other account reads, the first one is unaffected.
  await krRead(otherKr, b, 99, { epoch: now() + 3 });
  const unaffected = await fw.once("open-session", { ...bookArgs(b), pct: 42, xp: xp(42), page: 42 });
  t.ok(unaffected.decision.remote_device !== "Kobo Ajeno", "first account never hears of the other's reader");
  t.ok(unaffected.decision.remote_pct !== 99, "first account never hears of the other's position");
  const leaked = await h.sql`
    SELECT count(*)::int AS n FROM device_event_deliveries del JOIN devices d ON d.id = del.device_id
    JOIN device_events e ON e.id = del.event_id WHERE d.user_id <> e.user_id`;
  t.eq(leaked[0].n, 0, "no delivery ever crossed accounts");
}

export const SCENARIOS = [
  { id: 1, name: "Pairing: happy, reject, expiry, claim twice, same device row", run: pairing },
  { id: 2, name: "Resume: prompt, Go, Stay, reverse direction", run: resume },
  { id: 3, name: "Dedupe, own events, account isolation", run: dedupe },
];
