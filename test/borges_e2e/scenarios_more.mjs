// Scenarios 6 to 12.

import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { buildItem, comparableText } from "../borges_contract/koreader.mjs";
import * as B from "./books.mjs";
import { until } from "./device.mjs";
import { deviceWithBook, headIs, koreaderAt, wait } from "./scenarios.mjs";
import * as ui from "./ui.mjs";

const HANG = { action: "status", status: 200, match: "/api/sync/v2/pull", delayMs: 45000, times: 20 };

// ---------------------------------------------------------------------------------
// 6. Manual sync, shortcut, Settings, disconnect
// ---------------------------------------------------------------------------------

async function scenario6({ h, t, makeDevice }) {
  const { account, book, d } = await deviceWithBook(h, makeDevice);
  await ui.openFromHome(d);
  await d.tap("RIGHT", { settleMs: 200 });
  await d.tap("RIGHT", { settleMs: 200 });

  // Reader drawer: Sync now.
  h.proxy.clear();
  await ui.openLocationTab(d);
  await ui.drawerRow(d, ui.locationRows().sync);
  await d.shot("drawer-sync-focused");
  let boots = d.helloCount;
  let mark = d.mark();
  await d.press("CONFIRM");
  await d.waitBoot(boots);
  await d.waitLog(/Entering activity: Borges\b/, { since: mark });
  await until("the sync to reach the server", () => h.proxy.requestsTo("/api/devices/self/health").some((e) => e.status > 0), 30000);
  await d.waitLog(/Entering activity: EpubReader/, { since: mark, timeoutMs: 30000 });
  await d.waitStable(800);
  await d.shot("drawer-sync-back-in-reader");
  let events = (await h.events(d.deviceId)).filter((e) => e.event_type === "progress.changed");
  t.ok(events.length >= 1, "Sync now pushed the position");
  t.eq(ui.progressOf(d, B)?.page, 2, "the reader comes back on the same page");

  // Assign the shortcut, by buttons, from the reader's drawer: Settings tab, Controls.
  await d.tap("CONFIRM");
  await d.tap("RIGHT", { settleMs: 200 });
  await d.tap("RIGHT", { settleMs: 200 });
  await d.shot("drawer-settings-tab");
  await d.tap("DOWN", { settleMs: 200 });
  await d.tap("DOWN", { settleMs: 200 });
  await d.shot("drawer-controls-row");
  await d.tap("CONFIRM", { settleMs: 400 });
  await d.shot("controls");
  await d.tap("CONFIRM", { settleMs: 400 });
  await d.shot("power-button");
  await d.tap("CONFIRM", { settleMs: 400 });
  // Ignore, Sleep/Wake, ... "Sync now" is the sixteenth, under "Sync Progress".
  for (let i = 0; i < 15; i += 1) await d.tap("DOWN", { settleMs: 100 });
  await d.shot("short-press-picker-sync-now");
  await d.tap("CONFIRM", { settleMs: 400 });
  await d.shot("short-press-assigned");
  await d.tap("BACK", { settleMs: 300 });
  await d.tap("BACK", { settleMs: 300 });
  await d.shot("after-settings-back");
  await d.tap("BACK", { settleMs: 500 });
  await d.shot("reader-before-shortcut");
  t.note("assignable through the UI, per SettingsList.h: Short-press and Long-press of Power, the power chord, Up and Down side buttons short and long, the side chord, Long-press Back and Long-press Menu, Quick Actions. Exercised here: Short-press Power");
  const settingsJson = JSON.parse(fs.readFileSync(d.file("/.crosspoint/crossink-settings.json"), "utf8"));
  t.eq(settingsJson.shortPwrBtn, 37, "the setting on the card is BORGES_SYNC");

  // The shortcut itself, from the reader.
  await d.tap("RIGHT", { settleMs: 200 });
  h.proxy.clear();
  boots = d.helloCount;
  mark = d.mark();
  await d.press("POWER", 80);
  await d.waitBoot(boots);
  await d.waitLog(/Entering activity: Borges\b/, { since: mark });
  await until("the shortcut's sync", () => h.proxy.requestsTo("/api/devices/self/health").some((e) => e.status > 0), 30000);
  await d.waitLog(/Entering activity: EpubReader/, { since: mark, timeoutMs: 30000 });
  await d.waitStable(800);
  await d.shot("shortcut-back-in-reader");
  events = (await h.events(d.deviceId)).filter((e) => e.event_type === "progress.changed");
  t.eq(ui.progressOf(d, B)?.page, 3, "the shortcut came back on the same page");
  t.ok(events.length >= 2, `the shortcut pushed the new position (${events.length} events)`);

  // Settings, Borges.
  await ui.readerToHome(d);
  const inSettings = await ui.homeToSettings(d);
  t.ok(inSettings, "Settings can be reached from Home with the buttons");
  if (inSettings) {
    await d.shot("settings-root");
    t.skip("Settings, System tab, Borges: the row was reachable in this build but the walk to it is not scripted yet");
    await d.tap("BACK", { settleMs: 500 });
  } else {
    t.skip("Settings, System tab, Borges: Home cannot focus Settings in this build (see the failed check above), and there is no other way there with buttons");
    await d.shot("home-settings-out-of-reach");
  }

  // Something queued, then Disconnect from Home, Borges.
  ui.letThrottleExpire(d);
  await ui.openFromHome(d);
  await d.tap("RIGHT", { settleMs: 200 });
  await ui.readerToHome(d);
  await ui.homeToBorges(d);
  await d.shot("status-with-pending");
  const before = (await h.events(d.deviceId)).length;
  for (let i = 0; i < 4; i += 1) await d.tap("DOWN", { settleMs: 200 });
  await d.shot("status-disconnect-focused");
  await d.tap("CONFIRM", { settleMs: 400 });
  await d.shot("disconnect-confirmation");
  // First the way out: its default row keeps it.
  await d.tap("CONFIRM", { settleMs: 400 });
  await d.shot("disconnect-cancelled");
  t.ok(fs.existsSync(d.file("/.crosspoint/borges/cred.a")) || fs.existsSync(d.file("/.crosspoint/borges/cred.b")), "Back keeps the credential");
  // The dialog opens on Cancel, and Confirm is below it.
  await d.tap("CONFIRM", { settleMs: 400 });
  await d.tap("DOWN", { settleMs: 200 });
  await d.shot("disconnect-confirm-focused");
  await d.tap("CONFIRM", { settleMs: 500 });
  await d.shot("disconnected");
  const outbox = fs.readdirSync(d.file("/.crosspoint/borges/outbox"));
  t.ok(outbox.length > 0, `the queue stays on the card (${outbox.join(",")})`);
  const leftover = d.allFiles().filter((f) => fs.readFileSync(f).includes(Buffer.from(d.token)));
  t.eq(leftover.length, 0, `no plain token on the card after disconnecting (${leftover.map((f) => f.slice(d.dir.length)).join(",")})`);
  const [row] = await h.sql`SELECT status FROM devices WHERE id = ${d.deviceId}`;
  t.note(`the server's device row after Disconnect: ${row?.status}. It is local only: the server was not told, and the credential is not revoked`);

  // Pair again: same row, and the queue drains.
  h.proxy.clear();
  const firstId = d.deviceId;
  const claim = await ui.pairFromStatus(h, d, account);
  t.eq(claim.device.id, firstId, "re-pairing is the same device row");
  await d.shot("repaired-status");
  // The pairing ran the sync itself, so the queue drains without a press.
  await until("the drain", () => h.proxy.requestsTo("/api/devices/self/health").some((e) => e.status > 0), 30000);
  await wait(2000);
  await d.waitStable(500);
  await d.shot("repaired-after-sync");
  const after = await h.events(d.deviceId);
  t.ok(after.length > before, `the queued progress went out after re-pairing (${before} -> ${after.length})`);
  t.ok(after.every((e) => e.processing_status !== "rejected"), `nothing rejected (${after.map((e) => e.rejection_code).filter(Boolean).join(",")})`);
  t.ok(await headIs(h, book, account, d.deviceId), "and the device is the head");
}

// ---------------------------------------------------------------------------------
// 7. Offline
// ---------------------------------------------------------------------------------

async function scenario7({ h, t, makeDevice }) {
  const { account, book, d } = await deviceWithBook(h, makeDevice);
  await h.proxy.down();

  // Connection refused: opening a book.
  const opened = await ui.openFromHome(d);
  t.eq(opened.reached, "EpubReader", "no server: the book opens anyway");
  t.note(`refused: press to reader ${opened.elapsedMs} ms, route ${opened.acts.join(",")}`);
  t.ok(opened.acts.includes("BorgesOpen"), "it did try");
  t.ok(opened.elapsedMs < 16000, `within the open budget (${opened.elapsedMs} ms)`);
  await d.shot("offline-reader");
  for (let i = 0; i < 3; i += 1) await d.tap("RIGHT", { settleMs: 150 });

  // The failed check started a backoff (15 s, +/-20%): sleep does not even try.
  const backedOff = await ui.sleepAndWaitFlush(d);
  t.eq(backedOff.flushed, false, "right after a failure sleep skips the session");
  t.note(`refused, in backoff: sleep took ${backedOff.totalMs} ms, no boot`);

  // Wake in the backoff: straight to the book. When it has passed, sleep tries and has to
  // end by its deadline with nobody there.
  const wokeOffline = await ui.wake(d);
  t.eq(wokeOffline.reached, "EpubReader", "waking offline opens the book");
  t.note(`wake in backoff: route ${wokeOffline.acts.join(",")}`);
  await d.tap("RIGHT", { settleMs: 150 });
  await wait(19000);
  const flush = await ui.sleepAndWaitFlush(d);
  t.eq(flush.flushed, true, "with the backoff over sleep tries a session");
  t.note(`refused: flush boot to deep sleep ${flush.flushMs} ms`);
  t.ok(flush.flushMs < 19000, `sleeps by the 19 s hard deadline (${flush.flushMs} ms)`);
  t.eq((await h.events(d.deviceId)).filter((e) => e.event_type === "progress.changed").length, 0, "nothing reached the server");
  const pageWhenOffline = ui.progressOf(d, B)?.page;

  // The server back: everything drains.
  await h.proxy.up();
  h.proxy.clear();
  const woke = await ui.wake(d);
  t.eq(woke.reached, "EpubReader", "wake with the server back opens the book");
  t.note(`wake after the failures: route ${woke.acts.join(",")}, requests ${h.proxy.log.length}`);
  await d.shot("offline-back-reader");
  await ui.readerToHome(d);
  await ui.syncNowFromHome(h, d);
  await d.shot("offline-back-status");
  const events = (await h.events(d.deviceId)).filter((e) => e.event_type === "progress.changed");
  t.ok(events.length >= 1, "the queued progress drained");
  t.eq(ui.progressOf(d, B)?.page, pageWhenOffline, "the page is where it was left");
  t.ok(await headIs(h, book, account, d.deviceId), "and it made the device the head");
  await ui.statusToHome(d);

  // A server that accepts and never answers: the budget has to end it, not the socket.
  ui.letThrottleExpire(d);
  h.proxy.clear();
  h.proxy.fault(HANG);
  const hung = await ui.openFromHome(d, { timeoutMs: 40000 });
  t.eq(hung.reached, "EpubReader", "a hung server: the book opens anyway");
  t.note(`hung: press to reader ${hung.elapsedMs} ms, route ${hung.acts.join(",")}`);
  t.ok(hung.elapsedMs < 20000, `a hung server does not hold the reader beyond budget (${hung.elapsedMs} ms)`);
  await d.tap("RIGHT", { settleMs: 200 });
  await wait(19000);
  const hungFlush = await ui.sleepAndWaitFlush(d);
  t.note(`hung: flushed ${hungFlush.flushed}, flush boot to deep sleep ${hungFlush.flushMs} ms`);
  t.ok(hungFlush.flushMs < 20000, `a hung server does not hold sleep beyond its deadline (${hungFlush.flushMs} ms)`);
  h.proxy.clear();
}

// ---------------------------------------------------------------------------------
// 8. Any button skips
// ---------------------------------------------------------------------------------

async function scenario8({ h, t, makeDevice }) {
  const { book, account, d } = await deviceWithBook(h, makeDevice);
  await koreaderAt(h, account, book, { percent: 50, chapter: 6 });
  for (const key of ["RIGHT", "BACK", "UP"]) {
    ui.letThrottleExpire(d);
    h.proxy.clear();
    h.proxy.fault(HANG);
    const mark = d.mark();
    await d.press("CONFIRM");
    await d.waitLog(/Entering activity: BorgesOpen/, { since: mark, timeoutMs: 20000 });
    await until("the check to be on its way", () => h.proxy.requestsTo("/api/sync/v2/pull").length > 0, 10000);
    await d.waitStable(200);
    await d.shot(`slow-check-before-${key}`);
    const pressed = Date.now();
    await d.press(key);
    await d.waitLog(/Entering activity: EpubReader/, { since: mark, timeoutMs: 15000 });
    const skippedMs = Date.now() - pressed;
    t.note(`${key} during a slow check: reader ${skippedMs} ms after the press`);
    t.ok(skippedMs < 3000, `${key} skips at once (${skippedMs} ms)`);
    t.eq(ui.activities(d.logSince(mark)).includes("BorgesResume"), false, `${key}: no prompt, the check was abandoned`);
    await d.waitStable(800);
    await d.shot(`slow-check-after-${key}`);
    await ui.readerToHome(d);
  }
  // Nothing was lost: with the server answering again it still asks.
  h.proxy.clear();
  ui.letThrottleExpire(d);
  const again = await ui.openFromHome(d);
  t.eq(again.reached, "BorgesResume", "after the skips the question is still open");
  await d.shot("prompt-after-skips");
}

// ---------------------------------------------------------------------------------
// 9. Highlights
// ---------------------------------------------------------------------------------

async function scenario9({ h, t, makeDevice }) {
  const { account, book, d } = await deviceWithBook(h, makeDevice);
  await ui.openFromHome(d);
  await d.tap("RIGHT", { settleMs: 200 });
  await d.tap("RIGHT", { settleMs: 300 });
  await d.shot("page-to-annotate");
  await ui.addBookmark(d);
  await d.shot("bookmark-added");
  await ui.createClipping(d, { hasBookmarks: true });
  await d.shot("clipping-created");
  await d.tap("RIGHT", { settleMs: 300 });
  await ui.sleepAndWaitFlush(d);

  const [doc] = await h.sql`SELECT id, title, author, file, book_hash FROM documents WHERE user_id = ${account.id} AND book_hash = ${book.md5}`;
  t.ok(doc, "the document appears on the server");
  t.eq(doc?.title, book.title, "with its title");
  t.eq(doc?.author, book.author, "and author");
  const entries = await h.sql`SELECT id, text, chapter, color, drawer, page, xpointer FROM entries WHERE document_id = ${doc?.id ?? null} ORDER BY drawer`;
  t.eq(entries.length, 2, "a highlight and a bookmark");
  const highlight = entries.find((e) => e.drawer !== "bookmark");
  const bookmark = entries.find((e) => e.drawer === "bookmark");
  t.note(`highlight: ${JSON.stringify(highlight)}`);
  t.note(`bookmark: ${JSON.stringify(bookmark)}`);
  t.ok(highlight?.text?.length > 10, "the highlight has its text");
  t.eq(highlight?.chapter, "Capitulo 1", "and its chapter");
  t.eq(highlight?.color, "gray", "gray");
  // The page starts with the tail of paragraph 6, the first full one is 7, and the
  // selection starts at the page's first word that the cursor lands on.
  t.ok(highlight?.xpointer?.startsWith("/body/DocFragment[1]/"), `an xpointer in that chapter: ${highlight?.xpointer}`);
  const paragraph = Number(highlight?.xpointer?.match(/p\[(\d+)\]/)?.[1]);
  t.note(`highlight xpointer says paragraph ${paragraph}; on screen the highlight sits in "parrafo 7" (see clipping-created)`);
  t.eq(paragraph, 7, "the xpointer names the paragraph the highlighted words are in");
  t.ok(bookmark?.text?.length > 0, "the bookmark has its snippet");
  // Its snippet is the page's first words, the tail of paragraph 6.
  t.eq(Number(bookmark?.xpointer?.match(/p\[(\d+)\]/)?.[1]), 6, `the bookmark's xpointer names the paragraph its snippet is in: ${bookmark?.xpointer}`);
  t.eq(bookmark?.chapter, "Capitulo 1", "and its chapter");
  t.eq(bookmark?.drawer, "bookmark", "marked as a bookmark");

  // Idempotence: another session changes nothing.
  const annEvents = async () => (await h.events(d.deviceId)).filter((e) => /annotation|bookmark/.test(e.event_type));
  const before = await annEvents();
  t.eq(before.length, 2, `two annotation events (${before.map((e) => e.event_type).join(",")})`);

  // What KOReader needs to place the highlight: both ends, and the text crengine reads there.
  const created = before.find((e) => e.event_type === "annotation.created");
  const pos0 = created?.payload?.position?.pos0;
  const pos1 = created?.payload?.position?.pos1;
  t.ok(pos0 && pos1, `the highlight has both ends: ${pos0} .. ${pos1}`);
  t.eq(pos0, created?.payload?.xpointer, "and starts at its xpointer");
  const buildItemResult = buildItem(created?.payload ?? {}, "rolling", (x) => typeof x === "string" && x.startsWith("/body/"), 1);
  t.ok(buildItemResult.item, `annotationadapter.lua's buildItem takes it: ${buildItemResult.error}`);
  const oracle = process.env.BORGES_CRE_ORACLE;
  if (oracle && fs.existsSync(oracle) && pos0 && pos1) {
    // scripts/test-borges-xpointer.ps1 builds this: KOReader's real engine, reading the range.
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "cre-"));
    const epub = path.join(dir, "book.epub");
    fs.writeFileSync(epub, book.bytes);
    fs.writeFileSync(path.join(dir, "in"), `${pos0}\t${pos1}\n`);
    execFileSync(oracle, [epub, "--ranges", path.join(dir, "in"), "--out", path.join(dir, "out")], { stdio: "ignore" });
    const [, valid, read] = fs.readFileSync(path.join(dir, "out"), "utf8").trimEnd().split("\t");
    t.eq(valid, "1", "crengine accepts both ends");
    t.eq(comparableText((read ?? "").replace(/\\n/g, "\n")), comparableText(created?.payload?.text), "and reads there the text that was sent");
    fs.rmSync(dir, { recursive: true, force: true });
  } else {
    t.note("BORGES_CRE_ORACLE not set: the range was not read back with crengine");
  }
  t.ok(before.every((e) => e.processing_status === "accepted" || e.processing_status === "applied"), `accepted (${before.map((e) => e.processing_status).join(",")})`);
  h.proxy.clear();
  ui.letThrottleExpire(d);
  await ui.wake(d);
  await d.tap("RIGHT", { settleMs: 300 });
  await ui.sleepAndWaitFlush(d);
  t.eq((await annEvents()).length, before.length, "another session sends no annotation again");
  t.eq((await h.sql`SELECT count(*)::int FROM entries WHERE document_id = ${doc?.id ?? null}`)[0].count, 2, "and the entries stay two");

  // Delete both, through the UI.
  ui.letThrottleExpire(d);
  await ui.wake(d);
  await d.tap("LEFT", { settleMs: 300 });
  await d.tap("LEFT", { settleMs: 300 });
  await d.shot("back-on-annotated-page");
  await ui.removeBookmark(d, { hasBookmarks: true, hasClippings: true });
  await d.shot("bookmark-removed");
  await ui.openLocationTab(d);
  await d.shot("drawer-after-bookmark-delete");
  await ui.drawerRow(d, ui.locationRows({ hasBookmarks: false, hasClippings: true }).viewClippings);
  const listMark = d.mark();
  await d.press("CONFIRM");
  await d.waitLog(/Entering activity: \w*Clip\w*/, { since: listMark });
  await d.waitStable(500);
  await d.shot("clipping-list");
  // A long Confirm opens the menu with Delete.
  const menuFrames = d.frames;
  await d.press("CONFIRM", 1500);
  await d.waitNewFrame(menuFrames);
  await d.waitStable(500);
  await d.shot("clipping-menu");
  await d.tap("CONFIRM", { settleMs: 500 });
  await d.shot("clipping-deleted");
  await ui.sleepAndWaitFlush(d);
  const states = await h.sql`SELECT sync_id, deleted_at, revision::text FROM annotation_sync_state WHERE document_id = ${doc?.id ?? null}`;
  t.note(`annotation states: ${JSON.stringify(states.map((s) => ({ deleted: s.deleted_at !== null, rev: s.revision })))}`);
  t.ok(states.length === 2 && states.every((s) => s.deleted_at !== null), "both are deleted on the server");
  const deleted = (await h.events(d.deviceId)).filter((e) => /deleted/.test(e.event_type));
  t.eq(deleted.length, 2, `a delete event for each (${deleted.map((e) => e.event_type).join(",")})`);
}

// ---------------------------------------------------------------------------------
// 10. Crash-loop guard, power loss during the flush
// ---------------------------------------------------------------------------------

async function scenario10({ h, t, makeDevice }) {
  const { d } = await deviceWithBook(h, makeDevice);

  // Open a book, dying inside the network boot, again and again, with the pending record on
  // the card. A person retries: Home, the book.
  const attempts = [];
  for (let attempt = 1; attempt <= 4; attempt += 1) {
    h.proxy.clear();
    // The check never answers, so the boot is alive when the power goes.
    h.proxy.fault(HANG);
    const mark = d.mark();
    await d.press("CONFIRM");
    const hit = await d.waitLog(/Entering activity: (BorgesOpen|EpubReader)/, { since: mark, timeoutMs: 20000 });
    attempts.push(hit[1]);
    if (hit[1] === "EpubReader") break;
    await d.kill();
    await ui.bootToHome(d);
  }
  t.note(`open attempts, killed in each network boot: ${attempts.join(" -> ")}`);
  t.eq(attempts.at(-1), "EpubReader", "the reader opens once the guard gives up on the session");
  t.ok(attempts.length <= 3, `after at most two network boots that died (${attempts.length} presses)`);
  await d.waitStable(600);
  await d.shot("reader-after-crash-loop");

  // Waking into this book, dying each time. Enter the wake by a real sleep, first time.
  await d.tap("RIGHT", { settleMs: 200 });
  h.proxy.clear();
  await ui.sleepAndWaitFlush(d);
  ui.letThrottleExpire(d);
  const wakes = [];
  for (let attempt = 1; attempt <= 4; attempt += 1) {
    h.proxy.clear();
    h.proxy.fault(HANG);
    await d.kill();
    const mark = d.mark();
    const boots = d.helloCount;
    await d.start({ wakeReason: "power" });
    await d.waitBoot(boots);
    const hit = await d.waitLog(/Entering activity: (BorgesOpen|EpubReader|Home)/, { since: mark, timeoutMs: 20000 });
    wakes.push(hit[1]);
    if (hit[1] !== "BorgesOpen") break;
  }
  t.note(`wake attempts, killed in each network boot: ${wakes.join(" -> ")}`);
  t.ok(wakes.at(-1) !== "BorgesOpen" && wakes.length <= 3, `a wake that keeps dying stops trying the session (${wakes.join(" -> ")})`);
  await d.waitStable(800);
  await d.shot("after-wake-crash-loop");

  // Power loss during the sleep flush: whichever boot comes next is not stuck.
  await d.kill();
  ui.letThrottleExpire(d);
  h.proxy.clear();
  await ui.bootToHome(d);
  const reopened = await ui.openFromHome(d);
  t.eq(reopened.reached, "EpubReader", "the reader is reachable");
  await d.tap("RIGHT", { settleMs: 200 });
  const flushes = [];
  for (let attempt = 1; attempt <= 3; attempt += 1) {
    h.proxy.clear();
    h.proxy.fault({ ...HANG, match: /\/api\/sync\/v2\// });
    const boots = d.helloCount;
    const mark = d.mark();
    await d.press("SLEEP");
    await d.waitBoot(boots, 15000).catch(() => {});
    const flushed = await d.waitLog(/Entering activity: BorgesSleepFlush/, { since: mark, timeoutMs: 5000 }).then(
      () => true,
      () => false
    );
    flushes.push(flushed ? "flush killed" : "no flush (guard or plain sleep), killed");
    await d.kill();
    const next = d.mark();
    h.proxy.clear();
    await d.start({ wakeReason: "power" });
    const hit = await d.waitLog(/Entering activity: (BorgesOpen|EpubReader|Home)/, { since: next, timeoutMs: 20000 });
    flushes.push(`next boot ${hit[1]}`);
    if (hit[1] !== "Home") await d.waitLog(/Entering activity: (EpubReader|BorgesResume)/, { since: next, timeoutMs: 30000 });
    await d.waitStable(500);
    if (hit[1] === "Home") await ui.openFromHome(d);
    await d.tap("RIGHT", { settleMs: 200 });
  }
  t.note(`power loss sequence: ${flushes.join(" -> ")}`);
  t.eq(flushes.length, 6, "every boot after a power loss reached a screen");
  await d.shot("after-power-loss");

  // And everything still goes out.
  h.proxy.clear();
  await ui.sleepAndWaitFlush(d);
  const progress = (await h.events(d.deviceId)).filter((e) => e.event_type === "progress.changed");
  t.ok(progress.length >= 1, `the position reached the server in the end (${progress.length} events)`);
}

// ---------------------------------------------------------------------------------
// 11. First-time Wi-Fi picker
// ---------------------------------------------------------------------------------

async function scenario11({ t }) {
  t.skip(
    "the Borges path to WifiSelectionActivity needs !hasSavedWifi() && !link.connected(). In the simulator hasSavedWifi() is true whenever BORGES_SIM_BASE_URL is set (src/borges/BorgesStack.cpp:112) and BorgesLink::connected() is always true (src/borges/net/BorgesLink.cpp:52), and without BORGES_SIM_BASE_URL the firmware would talk to production"
  );
  t.skip(
    "a saved network out of range: BORGES_SIM_NO_WIFI does nothing on a Borges session, because BorgesStack::bringUp asks link.connected() first and the simulator's answers true, so connect() and its check of that variable never run"
  );
}

// ---------------------------------------------------------------------------------
// 12. Legacy import
// ---------------------------------------------------------------------------------

async function scenario12({ h, t, makeDevice }) {
  const account = await h.account("legado");
  const mac = Buffer.from([0x02, 0x42, 0x4f, 0x52, 0x47, 0x53]);
  const externalId = "x4-5f85f6a4b21ceeff";
  // A credential the server really issued, to the device as the old firmware paired it.
  const old = await h.koreader(account, { platform: "crossink-x4", externalId, name: "X4", model: "1.4.2-borges.1" });
  const token = Buffer.from(old.token);
  const obfuscated = Buffer.from(token.map((b, i) => b ^ mac[i % mac.length]));
  const d = makeDevice();
  fs.mkdirSync(d.file("/.borges"), { recursive: true });
  // borges.invalid can never resolve: the override must be what carries the traffic.
  fs.writeFileSync(d.file("/.borges/borges.json"), JSON.stringify({ baseUrl: "https://borges.invalid/", deviceId: old.deviceId, token_obf: obfuscated.toString("base64") }));

  await ui.bootToHome(d);
  await ui.homeToBorges(d);
  await d.shot("status-after-import");
  t.ok(fs.existsSync(d.file("/.crosspoint/borges/cred.a")) || fs.existsSync(d.file("/.crosspoint/borges/cred.b")), "the import wrote the new credential");
  h.proxy.clear();
  const boots = d.helloCount;
  const mark = d.mark();
  await d.press("CONFIRM"); // First row when paired is Sync now, not Connect.
  await d.waitBoot(boots);
  await d.waitLog(/Entering activity: Borges\b/, { since: mark });
  await until("the sync", () => h.proxy.requestsTo("/api/devices/self/health").some((e) => e.status > 0), 30000);
  t.eq(h.proxy.requestsTo("/api/devices/pairing/request").length, 0, "no pairing was asked for");
  const auth = h.proxy.log.find((e) => e.headers.authorization)?.headers.authorization;
  t.eq(auth, `Bearer ${old.token}`, "it talks with the imported credential");
  await wait(2500);
  await d.waitStable(500);
  await d.shot("status-after-sync");
  const rows = await h.sql`SELECT id, status, last_sync_at, client_version FROM devices WHERE user_id = ${account.id}`;
  t.eq(rows.length, 1, "still one device row");
  t.eq(rows[0]?.id, old.deviceId, "the same one");
  t.note(`device row after import: ${JSON.stringify(rows[0])}`);
  t.note(`legacy file still on the card after the import: ${fs.existsSync(d.file("/.borges/borges.json"))}`);
}

export const MORE_SCENARIOS = [
  { id: 6, name: "Manual: drawer, shortcut, Settings, disconnect, re-pair", run: scenario6 },
  { id: 7, name: "Offline: budgets, no modal, sleep deadline, drain", run: scenario7 },
  { id: 8, name: "Any button skips a slow check", run: scenario8 },
  { id: 9, name: "Highlights through the reader UI", run: scenario9 },
  { id: 10, name: "Crash-loop guard, power loss during flush", run: scenario10 },
  { id: 11, name: "First-time Wi-Fi picker", run: scenario11 },
  { id: 12, name: "Legacy credential import", run: scenario12 },
];

