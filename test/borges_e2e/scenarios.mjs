// The scenarios, in the order of the brief. A check that fails here is a finding about the
// firmware or the harness, and a person looks at every screenshot named in the run.

import fs from "node:fs";
import path from "node:path";
import { koreaderPartialMd5, sha256 } from "../borges_contract/epub.mjs";
import * as B from "./books.mjs";
import { until } from "./device.mjs";
import * as ui from "./ui.mjs";

export const wait = (ms) => new Promise((r) => setTimeout(r, ms));

// ---------------------------------------------------------------------------------
// Shared steps
// ---------------------------------------------------------------------------------

export function bookFixture(title = "Ficciones grandes", author = "Jorge Luis Borges", fileName = "ficciones.epub", padBytes = 1500000, shape = {}) {
  const bytes = B.buildBook({ title, author, padBytes, ...shape });
  return { title, author, fileName, bytes, md5: koreaderPartialMd5(bytes), sha: sha256(bytes) };
}

// A paired device with the big book downloaded through its own library screen, opened
// once and left, so Home has it as the recent book.
export async function deviceWithBook(h, makeDevice, name = "x4", opts = {}) {
  const account = await h.account(opts.prefix ?? "lector");
  const book = opts.book ?? bookFixture();
  await h.upload(account, book.fileName, book.bytes);
  const d = makeDevice(name, opts.device);
  await ui.freshPaired(h, d, account);
  await ui.statusToLibrary(d);
  await ui.waitLibraryList(h, d);
  await ui.downloadFocused(h, d);
  const mark = d.mark();
  await d.press("CONFIRM");
  await d.waitLog(/Entering activity: EpubReader/, { since: mark, timeoutMs: 40000 });
  await d.waitStable(800);
  await ui.readerToHome(d);
  ui.letThrottleExpire(d);
  h.proxy.clear();
  return { account, book, d };
}

export async function koreaderAt(h, account, book, { percent, chapter, paragraph = 1, kr = null, offsetSec = 0, name = "Kobo Libra" }) {
  const client = kr ?? (await h.koreader(account, { name }));
  client.enqueueProgress({
    book: book.md5,
    page: Math.round(percent * 10),
    totalPages: 1000,
    xpointer: B.xpointerOf(chapter, paragraph),
    title: book.title,
    author: book.author,
    epoch: Math.floor(Date.now() / 1000) + offsetSec,
  });
  const result = await client.syncAll();
  if (result.pending !== 0) throw new Error("the KOReader client could not send its progress");
  return client;
}

const focusPulls = (h) => h.proxy.requestsTo("/api/sync/v2/pull").filter((e) => e.body.includes('"focus_only":true'));

export async function headIs(h, book, account, deviceId, timeoutMs = 3000) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const head = (await h.head(book.md5, account.id))[0];
    if (head?.device_id === deviceId) return head;
    if (Date.now() > deadline) return null;
    await wait(100);
  }
}

// ---------------------------------------------------------------------------------
// 1. Fresh SD, pairing, and where the token ends up
// ---------------------------------------------------------------------------------

function findXorObfuscated(file, token) {
  // The token XORed with a repeating 6-byte key: a stretch of the file where the XOR with
  // the token repeats with period 6. Nothing is assumed about where it sits.
  const bytes = fs.readFileSync(file);
  const t = Buffer.from(token);
  for (let start = 0; start + t.length <= bytes.length; start += 1) {
    const key = [];
    let good = true;
    for (let i = 0; i < t.length; i += 1) {
      const k = bytes[start + i] ^ t[i];
      if (i < 6) key.push(k);
      else if (key[i % 6] !== k) {
        good = false;
        break;
      }
    }
    if (good) return Buffer.from(key).toString("hex");
  }
  return null;
}

function tokenSightings(d, token) {
  const plain = Buffer.from(token);
  const base64 = Buffer.from(token).toString("base64");
  const hex = Buffer.from(token).toString("hex");
  const secret = token.replace(/^toto_/, "");
  return d
    .allFiles()
    .filter((f) => {
      const bytes = fs.readFileSync(f);
      return bytes.includes(plain) || bytes.includes(secret) || bytes.includes(base64) || bytes.includes(hex);
    })
    .map((f) => path.relative(d.sd, f));
}

async function scenario1({ h, t, makeDevice }) {
  const account = await h.account("pareo");
  const d = makeDevice();
  await ui.bootToHome(d);
  await d.shot("home-unpaired");
  t.eq(fs.existsSync(d.file("/.crosspoint/borges/cred.a")), false, "a fresh SD has no credential");
  await ui.homeToBorges(d);
  await d.shot("borges-not-connected");

  const shots = { code: "pair-code", done: "pair-finished-alone" };
  const claim = await ui.pairFromStatus(h, d, account, { shots, t });
  await d.shot("status-paired");
  t.note(`look at ${path.basename(shots.codePath)}: it must show ${ui.pairingCode(h)}`);

  const token = claim.credential.token;
  const [device] = await h.sql`SELECT id, platform, external_id, status, user_id FROM devices WHERE id = ${claim.device.id}`;
  t.eq(device?.platform, "crossink-x4", "the device row has the platform");
  t.eq(device?.user_id, account.id, "the device belongs to the approving account");
  t.ok(device?.external_id?.startsWith("x4-"), "external id has the x4 prefix");

  const status = h.proxy.requestsTo("/api/devices/pairing/status");
  t.ok(status.length >= 1, "the device polled for the approval by itself");
  t.ok(fs.existsSync(d.file("/.crosspoint/borges/cred.a")) || fs.existsSync(d.file("/.crosspoint/borges/cred.b")), "the credential file exists");

  const seen = tokenSightings(d, token);
  t.eq(seen.join(","), "", "the token in the clear (plain, base64, hex or without its prefix) anywhere on the SD, temp dir or logs");
  const key = ["cred.a", "cred.b"].map((name) => d.file(`/.crosspoint/borges/${name}`)).filter((f) => fs.existsSync(f)).map((f) => findXorObfuscated(f, token)).find(Boolean);
  t.ok(!key, "the credential file does not hold the token XORed with the chip MAC (it is boxed with a device key)");
  // Pairing runs the first sync by itself, with the radio it already has up, and ends on the library row.
  await until("the first sync, run by the pairing", () => h.proxy.requestsTo("/api/devices/self/health").some((e) => e.status > 0), 30000);
  await wait(500);
  t.note(`plain token after the first sync: ${JSON.stringify(tokenSightings(d, token))}`);
  await d.close();
  t.note("credential storage: no longer XOR with the chip MAC (see 'keep the token off the card')");
}

// ---------------------------------------------------------------------------------
// 2. Library
// ---------------------------------------------------------------------------------

const DOWNLOADS = /\/api\/library\/plugin\/books\/[^/]+\/download$/;

async function scenario2({ h, t, makeDevice }) {
  const account = await h.account("biblio");
  const big = bookFixture();
  const second = bookFixture("Grande dos", "Sara Gallardo", "grande-dos.epub", 1200000);
  const hostileTitle = 'Una historia <universal> de la "infamia" & ../../etc/passwd, con un título larguísimo que no entra ni en dos renglones ñandú';
  const hostile = { ...bookFixture(hostileTitle, "Autor con un nombre también bastante más largo de lo razonable", '../../etc/passwd<"hostil">.epub', 0) };
  await h.upload(account, big.fileName, big.bytes);
  await h.upload(account, second.fileName, second.bytes);
  const hostileRow = await h.upload(account, hostile.fileName, hostile.bytes);
  const small = [];
  for (let i = 0; i < 12; i += 1) {
    const b = bookFixture(`Libro ${String(i).padStart(2, "0")}`, "Autor de Prueba", `libro-${i}.epub`, 0);
    await h.upload(account, b.fileName, b.bytes);
    small.push(b);
  }
  t.note(`server stored the hostile file as original_filename ${JSON.stringify(hostileRow.file.original_filename)}`);

  const d = makeDevice();
  await ui.freshPaired(h, d, account);
  h.proxy.clear();
  await ui.statusToLibrary(d);
  await ui.waitLibraryList(h, d);
  await d.shot("list-page-1");
  const finalPath = (name) => d.file(`/Borges/${name}`);

  // Cancel mid-download, on a throttled connection.
  h.proxy.fault({ action: "throttle", match: DOWNLOADS, bytesPerSec: 250000 });
  await d.press("CONFIRM");
  await until("half of the throttled download", () => (h.proxy.requestsTo(DOWNLOADS)[0]?.sent ?? 0) > 500000);
  await until("the progress screen to move", () => d.frames > 0 && fs.existsSync(finalPath(`${big.fileName}.part`)), 10000);
  await wait(2700);
  await d.shot("download-progress");
  const framesBeforeCancel = d.frames;
  await d.press("BACK");
  await d.waitNewFrame(framesBeforeCancel);
  await d.waitStable(500);
  await d.shot("download-cancelled-list");
  const partSize = fs.statSync(finalPath(`${big.fileName}.part`)).size;
  t.ok(partSize > 0 && partSize < big.bytes.length, `a partial file stays after cancelling (${partSize} bytes)`);
  t.eq(fs.existsSync(finalPath(big.fileName)), false, "a cancelled download is not a book yet");

  // Resume with Range through the throttle.
  h.proxy.fault({ action: "throttle", match: DOWNLOADS, bytesPerSec: 2000000 });
  await ui.downloadFocused(h, d);
  const resume = h.proxy.requestsTo(DOWNLOADS).at(-1);
  t.eq(resume.headers.range, `bytes=${partSize}-`, "the resume asks for the rest");
  t.ok(resume.headers["if-range"]?.includes(big.sha), "the resume is conditional on the same file");
  t.eq(resume.status, 206, "the server answered partial content");
  await d.shot("download-done");
  t.eq(fs.existsSync(finalPath(big.fileName)) && ui.sha256File(finalPath(big.fileName)), big.sha, "the downloaded bytes are the server's");
  t.eq(fs.existsSync(finalPath(`${big.fileName}.part`)), false, "no partial file is left");

  // Back to the list: the check mark.
  await d.tap("BACK");
  await d.shot("list-with-check-mark");

  // Connection dropped half way, then retried from the failure screen.
  await d.tap("DOWN", { settleMs: 200 });
  h.proxy.fault({ action: "cut", match: DOWNLOADS, fraction: 0.5 });
  const dropFrames = d.frames;
  await d.press("CONFIRM");
  await d.waitNewFrame(dropFrames);
  await until("the cut to happen", () => h.proxy.requestsTo(DOWNLOADS).at(-1).fault === "cut" && h.proxy.requestsTo(DOWNLOADS).at(-1).sent > 0);
  await ui.waitDownloadEnded(d, dropFrames);
  await d.shot("download-connection-dropped");
  t.eq(fs.existsSync(finalPath(second.fileName)), false, "a dropped connection does not leave a book");
  const dropped = fs.existsSync(finalPath(`${second.fileName}.part`)) ? fs.statSync(finalPath(`${second.fileName}.part`)).size : 0;
  t.ok(dropped > 0, `what arrived before the drop is kept (${dropped} bytes)`);
  await ui.downloadFocused(h, d);
  t.eq(h.proxy.requestsTo(DOWNLOADS).at(-1).status, 206, "the retry continues where it stopped");
  t.eq(fs.existsSync(finalPath(second.fileName)) && ui.sha256File(finalPath(second.fileName)), second.sha, "the retried file is identical");
  await d.tap("BACK");

  // Corruption inside a range the partial MD5 reads.
  await d.tap("DOWN", { settleMs: 200 });
  h.proxy.fault({ action: "corrupt", match: DOWNLOADS, offsets: [100] });
  await ui.downloadFocused(h, d);
  await d.shot("download-corrupt-sampled");
  t.eq(fs.existsSync(finalPath(small[0].fileName)), false, "a file failing the integrity check is not accepted");
  t.eq(fs.existsSync(finalPath(`${small[0].fileName}.part`)), false, "the corrupt partial is not kept to resume from");
  // The failure screen offers the retry: this time the bytes are good.
  await ui.downloadFocused(h, d);
  await d.shot("download-corrupt-retry-ok");
  t.eq(fs.existsSync(finalPath(small[0].fileName)) && ui.sha256File(finalPath(small[0].fileName)), small[0].sha, "the retry after corruption is identical");
  await d.tap("BACK");

  // Corruption the partial MD5 cannot see: 1024*2 to 1024*4 is never sampled. Only a
  // whole-file hash catches this.
  await d.tap("DOWN", { settleMs: 200 });
  h.proxy.fault({ action: "corrupt", match: DOWNLOADS, offsets: [3000] });
  await ui.downloadFocused(h, d);
  await d.shot("download-corrupt-unsampled");
  const silently = fs.existsSync(finalPath(small[1].fileName));
  t.ok(!silently || ui.sha256File(finalPath(small[1].fileName)) === small[1].sha, "corruption outside the partial MD5's samples is not accepted (the server sends X-Content-SHA256 too)");
  await d.tap("BACK");

  // Page 2, and the hostile title. Eleven rows on a page, Libro 01 is row 3: seven Downs
  // reach the last row and the eighth asks for the next page.
  h.proxy.log.length = 0;
  for (let i = 0; i < 7; i += 1) await d.tap("DOWN", { settleMs: 100 });
  await d.press("DOWN");
  await ui.waitLibraryList(h, d, 2);
  await d.shot("list-page-2");
  const page2 = JSON.parse(h.proxy.requestsTo(/page=2&/).at(-1).responseBody);
  t.ok(page2.books.some((b) => b.title === hostileTitle), "the hostile title is on page 2");
  const index = page2.books.findIndex((b) => b.title === hostileTitle);
  await ui.libraryFocusTo(d, index);
  await d.shot("list-hostile-focused");
  await ui.downloadFocused(h, d);
  await d.shot("hostile-downloaded");
  const borges = fs.readdirSync(d.file("/Borges"));
  const hostileFile = borges.find((f) => f.endsWith(".epub") && ui.sha256File(finalPath(f)) === hostile.sha);
  t.ok(hostileFile, `the hostile book landed inside /Borges (saw ${JSON.stringify(borges)})`);
  t.ok(hostileFile && !hostileFile.includes("..") && !/[<>"\\/]/.test(hostileFile), `its file name is safe: ${hostileFile}`);
  t.eq(fs.existsSync(d.file("/etc")), false, "nothing escaped to /etc");

  // Open it from the list.
  const mark = d.mark();
  await d.press("CONFIRM");
  await d.waitLog(/Entering activity: EpubReader/, { since: mark, timeoutMs: 40000 });
  await d.waitStable(800);
  await d.shot("hostile-open-reader");
}

// ---------------------------------------------------------------------------------
// 3. KOReader to the device: Go, and Undo
// ---------------------------------------------------------------------------------

async function scenario3({ h, t, makeDevice }) {
  const { account, book, d } = await deviceWithBook(h, makeDevice);
  await d.shot("home-with-book");

  const kr = await koreaderAt(h, account, book, { percent: 60, chapter: 7, paragraph: 5 });
  h.proxy.clear();

  const opened = await ui.openFromHome(d);
  t.eq(opened.reached, "BorgesResume", "the open stops at the prompt");
  t.ok(opened.acts.includes("BorgesHandoff") || opened.acts.includes("BorgesOpen"), `the session ran (${opened.acts.join(",")})`);
  t.note(`prompt on screen ${opened.elapsedMs} ms after the press (real network boot of the simulator)`);
  await d.shot("prompt-60-percent");
  const focus = focusPulls(h)[0];
  t.ok(focus, "the check is a focus_only pull");
  t.ok(focus?.body.includes(book.md5), "for this edition");

  await ui.choose(d);
  await d.shot("after-go");
  const progress = ui.progressOf(d, B);
  t.eq(progress?.spine, 6, "the reader lands in DocFragment[7]");
  t.note(`progress.bin after Go: ${JSON.stringify(progress)}`);
  t.ok(progress && progress.page <= progress.pageCount, "the page exists in the chapter");
  if (progress?.pageCount) {
    const pct = ((progress.spine + progress.page / progress.pageCount) / 10) * 100;
    t.near(pct, 60, 5, "the book position is near 60%");
  }

  // The jump goes out with the next session: at Go the radio is already off for the book
  // to load. Nothing else may be queued for this to be the event that goes.
  const immediately = await headIs(h, book, account, d.deviceId, 500);
  t.note(`head right after Go: ${immediately ? "this device" : "still KOReader, the event goes out with the next session"}`);
  const flush = await ui.sleepAndWaitFlush(d);
  t.note(`sleep flush took ${flush.flushMs} ms`);
  const progressEvents = () => h.events(d.deviceId).then((rows) => rows.filter((e) => e.event_type === "progress.changed"));
  const events = await progressEvents();
  t.note(`progress events: ${events.map((e) => `${Number(e.payload.percentage).toFixed(1)}%${e.payload.explicit_jump ? " jump" : ""} ${e.processing_status}`).join(", ")}`);
  const jump = events.find((e) => e.payload.explicit_jump === true);
  t.ok(jump, "the Go went out as explicit_jump");
  t.near(Number(jump?.payload.percentage), 60, 1, "with the remote percentage");
  t.eq(jump?.payload.xpointer, B.xpointerOf(7, 5), "and the xpointer it jumped to");
  const head = await headIs(h, book, account, d.deviceId);
  t.ok(head, "the device is the head afterwards");
  t.eq(head?.explicit_jump, true, "the state row says it was a jump");
  const seen = await kr.pull({ focusBook: book.md5, focusOnly: true });
  t.eq(seen.focusEvent?.origin_device?.id, d.deviceId, "KOReader sees the device as the head");

  // Wake into it: own event, nothing to ask. Undo from the drawer.
  ui.letThrottleExpire(d);
  const woke = await ui.wake(d);
  t.eq(woke.reached, "EpubReader", "its own event does not ask");
  await ui.openLocationTab(d);
  await d.shot("drawer-with-undo");
  await ui.drawerRow(d, ui.locationRows({ hasUndo: true }).undo);
  await d.shot("drawer-undo-focused");
  const undoMark = d.mark();
  await d.press("CONFIRM");
  await d.waitLog(/Entering activity: EpubReader/, { since: undoMark, timeoutMs: 30000 });
  await d.waitStable(800);
  await d.shot("after-undo");
  const undone = ui.progressOf(d, B);
  t.eq(undone?.spine, 0, "undo goes back to the chapter it left");
  t.eq(undone?.page, 0, "undo goes back to the page it left");

  // The undo is single use.
  await ui.openLocationTab(d);
  await d.shot("drawer-undo-gone");
  await d.tap("BACK");
}

// ---------------------------------------------------------------------------------
// 4. Stay
// ---------------------------------------------------------------------------------

async function scenario4({ h, t, makeDevice }) {
  const { account, book, d } = await deviceWithBook(h, makeDevice);
  const kr = await koreaderAt(h, account, book, { percent: 30, chapter: 4 });
  h.proxy.clear();

  const first = await ui.openFromHome(d);
  t.eq(first.reached, "BorgesResume", "KOReader further along asks");
  await d.shot("prompt-first");
  await d.tap("DOWN", { settleMs: 200 });
  await d.shot("prompt-stay-focused");
  const stayMark = d.mark();
  await d.press("CONFIRM");
  await d.waitLog(/Entering activity: EpubReader/, { since: stayMark, timeoutMs: 40000 });
  await d.waitStable(800);
  await d.shot("after-stay");
  t.eq(ui.progressOf(d, B)?.spine ?? 0, 0, "stay does not move the reader");
  const stayEvents = (await h.events(d.deviceId)).filter((e) => e.event_type === "progress.changed");
  const stayEvent = stayEvents.at(-1);
  t.ok(stayEvent, "stay sent its progress while the radio was up");
  t.eq(stayEvent?.payload.explicit_jump, false, "stay is not a jump");
  t.ok(await headIs(h, book, account, d.deviceId), "stay makes this device the head");

  // Next open: nothing new, no prompt.
  await ui.readerToHome(d);
  ui.letThrottleExpire(d);
  h.proxy.clear();
  const second = await ui.openFromHome(d);
  t.eq(second.reached, "EpubReader", "the next open does not ask again");
  t.ok(focusPulls(h).length >= 1, "it did check");
  t.eq(second.acts.includes("BorgesResume"), false, "no prompt on the way");

  // KOReader moves: asks again.
  await ui.readerToHome(d);
  ui.letThrottleExpire(d);
  await wait(1100);
  await koreaderAt(h, account, book, { percent: 40, chapter: 5, kr });
  h.proxy.clear();
  const third = await ui.openFromHome(d);
  t.eq(third.reached, "BorgesResume", "KOReader moved: it asks again");
  await d.shot("prompt-after-koreader-moved");
  await d.tap("DOWN", { settleMs: 200 });
  const m3 = d.mark();
  await d.press("CONFIRM");
  await d.waitLog(/Entering activity: EpubReader/, { since: m3, timeoutMs: 40000 });
  await d.waitStable(800);

  // The same spot in a new event: still a new event, it asks again.
  await ui.readerToHome(d);
  ui.letThrottleExpire(d);
  await wait(1100);
  await koreaderAt(h, account, book, { percent: 40, chapter: 5, kr });
  h.proxy.clear();
  const fourth = await ui.openFromHome(d);
  t.eq(fourth.reached, "BorgesResume", "the same spot in a new event asks again");
  await d.shot("prompt-same-spot-new-event");
  // Back answers nothing: opens where it was and asks again next time.
  const m4 = d.mark();
  await d.press("BACK");
  await d.waitLog(/Entering activity: EpubReader/, { since: m4, timeoutMs: 40000 });
  await d.waitStable(800);
  await ui.readerToHome(d);
  ui.letThrottleExpire(d);
  h.proxy.clear();
  const fifth = await ui.openFromHome(d);
  t.eq(fifth.reached, "BorgesResume", "Back did not answer: it asks again");
  await d.shot("prompt-after-back");
}

// ---------------------------------------------------------------------------------
// 5. Device to KOReader
// ---------------------------------------------------------------------------------

const iso = (date) => new Date(date).getTime();

async function scenario5({ h, t, makeDevice }) {
  const { account, book, d } = await deviceWithBook(h, makeDevice);
  const kr = await h.koreader(account);
  ui.letThrottleExpire(d);
  const opened = await ui.openFromHome(d);
  t.eq(opened.reached, "EpubReader", "nobody else read it: no prompt");
  await d.shot("reader-first-page");
  for (let i = 0; i < 6; i += 1) await d.tap("RIGHT", { settleMs: 150 });
  await d.shot("reader-page-7");
  const lastTurn = Date.now();
  // A gap between the last page turn and the sleep shows which of the two is reported.
  await wait(4000);
  h.proxy.clear();
  const flush = await ui.sleepAndWaitFlush(d);
  t.note(`sleep to deep sleep ${flush.totalMs} ms, flush boot ${flush.flushMs} ms`);
  const events = (await h.events(d.deviceId)).filter((e) => e.event_type === "progress.changed");
  const event = events.at(-1);
  t.ok(event, "the server has the progress");
  t.note(`event: ${JSON.stringify({ pct: event?.payload.percentage, xp: event?.payload.xpointer, at: event?.occurred_at, precision: event?.time_precision, page: event?.payload.current_page, of: event?.payload.total_pages })}`);
  t.near(iso(event?.occurred_at), lastTurn, 2500, "occurred_at is the last page turn, not the sleep");
  t.ok(["exact", "anchored"].includes(event?.time_precision), `precision is honest: ${event?.time_precision}`);
  t.ok(/^\/body\/DocFragment\[1\]\/body\/p\[\d+\]/.test(event?.payload.xpointer ?? ""), `a real xpointer: ${event?.payload.xpointer}`);
  t.eq(event?.payload.explicit_jump, false, "page turns are not a jump");
  const onDisk = ui.progressOf(d, B);
  t.note(`progress.bin: ${JSON.stringify(onDisk)}`);
  t.eq(onDisk?.page, 6, "the device saved page 7");
  const expectedPct = ((onDisk.page / onDisk.pageCount) / 10) * 100;
  t.near(Number(event?.payload.percentage), expectedPct, 1.5, "percentage agrees with the page");

  const seen = await kr.pull({ focusBook: book.md5, focusOnly: true });
  const decision = kr.decideHead(seen, book.md5);
  t.eq(decision.valid, true, "KOReader validates the event");
  t.eq(decision.ask, true, "KOReader would ask");
  t.eq(seen.focusEvent?.origin_device?.id, d.deviceId, "the device is the head for KOReader");
  t.eq(seen.focusEvent?.payload?.xpointer, event?.payload.xpointer, "with that xpointer");
  const head = await headIs(h, book, account, d.deviceId);
  t.ok(head, "the state row agrees");

  // Wake: the wake boot is the network boot, without another reboot before it.
  await koreaderAt(h, account, book, { percent: 45, chapter: 6, kr, offsetSec: 2 });
  h.proxy.clear();
  ui.letThrottleExpire(d);
  const woke = await ui.wake(d);
  t.eq(woke.acts[0], "BorgesOpen", `the wake boot goes straight to the check (${woke.acts.join(",")})`);
  t.eq(woke.acts.includes("BorgesHandoff"), false, "no handoff reboot on the way");
  t.eq(woke.reached, "BorgesResume", "and asks");
  t.eq(woke.boots, 1, "in one boot");
  await d.shot("wake-prompt");
}

export const SCENARIOS = [
  { id: 1, name: "Fresh SD, pairing, token storage", run: scenario1 },
  { id: 2, name: "Library: download, resume, corruption, drop, pages, hostile title", run: scenario2 },
  { id: 3, name: "KOReader to device: prompt, Go, Undo", run: scenario3 },
  { id: 4, name: "Stay, no re-prompt, new event re-prompts", run: scenario4 },
  { id: 5, name: "Device to KOReader: sleep flush, wake re-check", run: scenario5 },
];
