// How a person gets around the firmware, in buttons. Home's menu changes with whether a
// book has been opened, and the reader's drawer with what the book holds, so those two are
// computed from what the test knows rather than found on screen.

import { createHash } from "node:crypto";
import fs from "node:fs";

export const activities = (text) => [...text.matchAll(/Entering activity: (\w+)/g)].map((m) => m[1]);

export const sha256File = (file) => createHash("sha256").update(fs.readFileSync(file)).digest("hex");

export async function bootToHome(d, opts = {}) {
  const boots = d.helloCount;
  d.mark();
  await d.start(opts);
  await d.waitBoot(boots);
  await d.waitLog(/Entering activity: Home/);
  await d.waitStable(600);
}

// Home's menu grows in the middle (a recent book, Reading Stats, Bookmarks & Clippings) and
// always ends in Borges, File Transfer, Settings. Down wraps, and the focused row is the
// only one with a dither in its empty right margin, so the rows are counted on screen.
async function homeFocusY(d) {
  const screen = await d.pixels();
  for (let y = 50; y < 730; y += 4) {
    // The focus bar is a dither (0.25 here). The book card's bottom edge leaves 0.15 at y 238 in every state.
    if (screen.inkFraction(444, y, 456, y + 4) > 0.2) return y;
  }
  return -1;
}

// Walks Down until the focus wraps. Returns how many rows the buttons can reach, and whether
// a menu row is drawn below the last of them, which no button can then reach.
export async function measureHomeMenu(d) {
  const start = await homeFocusY(d);
  if (start < 0) throw new Error("Home: no focused row on screen");
  let count = 0;
  let last = start;
  for (;;) {
    await d.tap("DOWN", { settleMs: 200 });
    count += 1;
    const y = await homeFocusY(d);
    if (y < 0) throw new Error("Home: no focused row on screen");
    if (y === start) break;
    last = y;
    if (count > 12) throw new Error("Home: the focus never came back to the first row");
  }
  const screen = await d.pixels();
  const rowBelow = screen.inkFraction(36, last + 66, 66, last + 100) > 0.02;
  return { count, rowBelow };
}

async function homeRowsFromEnd(d) {
  const { count, rowBelow } = await measureHomeMenu(d);
  // Normally: ..., Borges, File Transfer, Settings. With a row out of reach, Settings is it.
  return { borges: rowBelow ? count - 2 : count - 3, settings: rowBelow ? null : count - 1 };
}

export async function homeToBorges(d) {
  const rows = await homeRowsFromEnd(d);
  for (let i = 0; i < rows.borges; i += 1) await d.tap("DOWN", { settleMs: 200 });
  d.mark();
  await d.press("CONFIRM");
  await d.waitLog(/Entering activity: Borges\b/);
  await d.waitStable(500);
}

export async function homeToSettings(d) {
  const rows = await homeRowsFromEnd(d);
  if (rows.settings === null) return false;
  for (let i = 0; i < rows.settings; i += 1) await d.tap("DOWN", { settleMs: 200 });
  d.mark();
  await d.press("CONFIRM");
  await d.waitLog(/Entering activity: Setting/);
  await d.waitStable(500);
  return true;
}

// Settings opens on its tab bar, on Display. Confirm moves along the tabs: the button says which is
// next. System is the last, and Borges its fifth row: Device, Files, Stats, Wi-Fi, Borges.
export async function settingsToBorges(d) {
  for (let i = 0; i < 3; i += 1) await d.tap("CONFIRM", { settleMs: 300 });
  await d.shot("settings-system-tab");
  for (let i = 0; i < 5; i += 1) await d.tap("DOWN", { settleMs: 200 });
  await d.shot("settings-borges-row");
  const mark = d.mark();
  await d.press("CONFIRM");
  await d.waitLog(/Entering activity: Borges\b/, { since: mark });
  await d.waitStable(500);
}

export function pairingCode(h) {
  const request = h.proxy.requestsTo("/api/devices/pairing/request").filter((e) => e.status === 201 || e.status === 200).at(-1);
  return request ? JSON.parse(request.responseBody).user_code : null;
}

// From the Borges status screen with its "Connect" row focused. Leaves the device on the
// status screen of the network boot, paired.
export async function pairFromStatus(h, d, account, { shots = {}, t, sameBoot = false } = {}) {
  const boots = d.helloCount;
  const before = h.proxy.requestsTo("/api/devices/pairing/claim").length;
  const requestsBefore = h.proxy.requestsTo("/api/devices/pairing/request").length;
  d.mark();
  await d.press("CONFIRM");
  if (!sameBoot) {
    await d.waitBoot(boots);
    await d.waitLog(/Entering activity: Borges\b/);
  }
  await until(() => h.proxy.requestsTo("/api/devices/pairing/request").length > requestsBefore && pairingCode(h), "the pairing request");
  await d.waitStable(500);
  const code = pairingCode(h);
  if (t) t.note(`code from the server: ${code}`);
  if (shots.code) shots.codePath = await d.shot(shots.code);
  const framesAtCode = d.frames;
  await h.approve(account, code);
  await until(() => h.proxy.requestsTo("/api/devices/pairing/claim").filter((e) => e.status === 200).length > before, "the claim", 30000);
  await d.waitNewFrame(framesAtCode);
  if (shots.done) shots.donePath = await d.shot(shots.done);
  const framesAtDone = d.frames;
  // The first sync's screen, then "Done" for 3 s, then the status. How many of those show up as a
  // new frame depends on how fast the sync goes. A press before the status dismisses "Done" and does
  // not reach the row the focus is on, so this waits out all of it.
  await until(() => h.proxy.requestsTo("/api/devices/self/health").some((e) => e.status > 0) || d.frames > framesAtDone + 2, "the first sync", 30000);
  await d.waitStable(3600, 40000);
  const claim = JSON.parse(h.proxy.requestsTo("/api/devices/pairing/claim").filter((e) => e.status === 200).at(-1).responseBody);
  return claim;
}

const until = async (fn, what, timeoutMs = 20000, stepMs = 50) => {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const v = await fn();
    if (v) return v;
    if (Date.now() > deadline) throw new Error(`timed out waiting for ${what}`);
    await new Promise((r) => setTimeout(r, stepMs));
  }
};

// A paired device back on a fresh Home, with no reader open.
export async function freshPaired(h, d, account) {
  await bootToHome(d);
  await homeToBorges(d);
  const claim = await pairFromStatus(h, d, account);
  d.deviceId = claim.device.id;
  d.token = claim.credential.token;
  return claim;
}

// Status screen of a network boot, rows: Sync now, Library, ... Back restarts to Home.
export async function statusToHome(d) {
  const boots = d.helloCount;
  d.mark();
  await d.press("BACK");
  await d.waitBoot(boots);
  await d.waitLog(/Entering activity: Home/);
  await d.waitStable(500);
}

// Status screen rows when paired: Sync now, Library, On open, On sleep, Disconnect. Pairing leaves the
// focus on Library already (it has just run the first sync); `down` is for a status that opens on row 0.
export async function statusToLibrary(d, { networkBoot = true, down = false } = {}) {
  if (down) await d.tap("DOWN", { settleMs: 200 });
  const boots = d.helloCount;
  d.mark();
  await d.press("CONFIRM");
  if (!networkBoot) await d.waitBoot(boots);
  await d.waitLog(/Entering activity: BorgesLibrary/);
}

// The list paints a moment after its answer, and a press on "Loading" goes nowhere. The
// focused row is a solid bar from the left edge, which no other screen of the library has.
async function libraryListOnScreen(d) {
  const screen = await d.pixels();
  for (let y = 90; y < 730; y += 8) {
    if (screen.darkFraction(2, y, 10, y + 8) > 0.9) return true;
  }
  return false;
}

export async function waitLibraryList(h, d, page = 1) {
  await until(() => h.proxy.requestsTo(new RegExp(`/api/library/plugin/books\\?page=${page}&`)).some((e) => e.status > 0), `library page ${page}`);
  const frames = d.frames;
  await until(async () => d.frames > frames || (await libraryListOnScreen(d)), "the list on screen", 15000);
  await until(() => libraryListOnScreen(d), "the list on screen", 15000);
  // A new page: the old list stays up through "Loading", which takes no buttons, and the answer has
  // to be parsed before the new one paints. A press before that is lost, and the next press on the
  // focused row downloads some other book.
  await d.waitStable(1500);
}

// The focused row is the solid bar. Looked at, not assumed: a press that lands while a page is still
// on its way is ignored, as it should be, and the Confirm after it would fetch the wrong book.
export async function libraryFocusTo(d, row, rowTop = 93, rowHeight = 59) {
  for (let press = 0; press < 12; press += 1) {
    const screen = await d.pixels();
    if (screen.darkFraction(2, rowTop + row * rowHeight + 10, 10, rowTop + row * rowHeight + 30) > 0.9) return;
    await d.press("DOWN");
    await d.waitStable(300);
  }
  throw new Error(`the library's focus never reached row ${row}`);
}

// The Downloading screen is the only one of the library with no label on its second button:
// that is how the harness knows a download ended, whichever way it ended.
export async function waitDownloadEnded(d, framesBefore, timeoutMs = 60000) {
  await d.waitNewFrame(framesBefore, timeoutMs);
  await until(async () => (await d.pixels()).inkFraction(155, 768, 215, 782) > 0.03, "the download screen to end", timeoutMs, 150);
  await d.waitStable(400);
}

// A download through the UI with the focused row, to its end.
export async function downloadFocused(h, d, { timeoutMs = 60000 } = {}) {
  const before = h.proxy.requestsTo(/\/download$/).length;
  const frames = d.frames;
  await d.press("CONFIRM");
  await until(() => h.proxy.requestsTo(/\/download$/).length > before && h.proxy.requestsTo(/\/download$/).at(-1).status > 0, "the download", timeoutMs);
  await waitDownloadEnded(d, frames, timeoutMs);
}

// Home, Borges, Sync now, through the handoff boot, until the result has gone and the status
// screen of that network boot is back. Every request was made by then.
export async function syncNowFromHome(h, d) {
  await homeToBorges(d);
  const boots = d.helloCount;
  const before = h.proxy.requestsTo("/api/devices/self/health").length;
  const mark = d.mark();
  await d.press("CONFIRM");
  await d.waitBoot(boots);
  await d.waitLog(/Entering activity: Borges\b/, { since: mark });
  await until(() => h.proxy.requestsTo("/api/devices/self/health").filter((e) => e.status > 0).length > before, "the health report", 40000);
  await new Promise((r) => setTimeout(r, 2000));
  await d.waitStable(500);
}

// Reader to Home. Finalizes the book locally and does not touch the network.
export async function readerToHome(d) {
  d.mark();
  await d.press("BACK");
  await d.waitLog(/Entering activity: Home/);
  await d.waitStable(500);
}

// Home's first row is the recent book. Waits for the reader of whichever route it takes,
// and returns the route. On a prompt it stops there.
export async function openFromHome(d, { until: stopAt = /Entering activity: (EpubReader|BorgesResume)/, timeoutMs = 45000 } = {}) {
  const started = Date.now();
  d.mark();
  const mark = d.logOffsetMark;
  await d.press("CONFIRM");
  const match = await d.waitLog(stopAt, { timeoutMs, since: mark });
  const elapsedMs = Date.now() - started;
  await d.waitStable(800);
  return { reached: match[1], elapsedMs, acts: activities(d.logSince(mark)), mark };
}

export async function choose(d, { stay = false } = {}) {
  if (stay) await d.tap("DOWN", { settleMs: 200 });
  const mark = d.mark();
  await d.press("CONFIRM");
  await d.waitLog(/Entering activity: EpubReader/, { since: mark, timeoutMs: 40000 });
  await d.waitStable(800);
}

// The drawer opens on "More", with that tab's icon filled. One to the right is location,
// and its icon, filled, is how the harness knows the press was not lost to a repaint.
const LOCATION_TAB = [310, 150, 350, 185];

export async function openLocationTab(d) {
  const mark = d.mark();
  await d.press("CONFIRM");
  await d.waitLog(/Entering activity: \w*Drawer/, { since: mark });
  await d.waitStable(300);
  await d.tap("RIGHT", { settleMs: 200 });
  const filled = (await d.pixels()).darkFraction(...LOCATION_TAB);
  if (filled < 0.5) throw new Error(`the drawer is not on its location tab (${filled.toFixed(2)})`);
}

// The bookmark row closes and reopens the drawer; Back is what leaves it.
async function toggleBookmarkRow(d) {
  const mark = d.mark();
  await d.press("CONFIRM");
  await d.waitLog(/Exiting activity: EpubReaderDrawer[\s\S]*Entering activity: EpubReaderDrawer/, { since: mark });
  await d.waitStable(500);
  const back = d.mark();
  await d.press("BACK");
  await d.waitLog(/Exiting activity: EpubReaderDrawer/, { since: back });
  await d.waitStable(600);
}

// Rows: Add/Remove Bookmark, [View Bookmarks, Clear], Create Clipping, [View Clippings],
// Sync now, Go to where another reader left off, [Undo], Nearby..., and the first Down only takes the focus to the first row.
export function locationRows({ hasBookmarks = false, hasClippings = false, hasUndo = false } = {}) {
  let i = 0;
  const rows = { bookmark: i++ };
  if (hasBookmarks) {
    rows.viewBookmarks = i++;
    rows.clearBookmarks = i++;
  }
  rows.clip = i++;
  if (hasClippings) rows.viewClippings = i++;
  rows.sync = i++;
  rows.otherDevice = i++;
  if (hasUndo) rows.undo = i++;
  return rows;
}

export async function drawerRow(d, index) {
  for (let i = 0; i <= index; i += 1) await d.tap("DOWN", { settleMs: 150 });
}

export async function addBookmark(d, flags = {}) {
  await openLocationTab(d);
  await drawerRow(d, locationRows(flags).bookmark);
  await toggleBookmarkRow(d);
}

export async function removeBookmark(d, flags = {}) {
  await addBookmark(d, flags);
}

export async function createClipping(d, flags = {}, words = 6) {
  await openLocationTab(d);
  await drawerRow(d, locationRows(flags).clip);
  const mark = d.mark();
  await d.press("CONFIRM");
  await d.waitLog(/Entering activity: \w*Clip\w*/, { since: mark });
  await d.waitStable(800);
  await d.tap("CONFIRM", { settleMs: 200 });
  for (let i = 0; i < words; i += 1) await d.tap("RIGHT", { settleMs: 150 });
  await d.tap("CONFIRM", { settleMs: 800 });
}

// Either route to deep sleep: through a flush boot, or straight there when the gate says no
// session now (backoff, no credential). Says which.
export async function sleepAndWaitFlush(d, { timeoutMs = 40000 } = {}) {
  const started = Date.now();
  const boots = d.helloCount;
  const mark = d.mark();
  await d.press("SLEEP");
  await until(() => d.helloCount > boots || /Entering deep sleep/.test(d.logSince(mark)), "the sleep to begin", 30000);
  if (d.helloCount === boots) return { flushed: false, totalMs: Date.now() - started, flushMs: 0, mark };
  await d.waitLog(/Entering activity: BorgesSleepFlush/, { since: mark, timeoutMs: 15000 });
  const flushAt = Date.now();
  await d.waitLog(/Entering deep sleep after Borges flush/, { since: mark, timeoutMs });
  return { flushed: true, totalMs: Date.now() - started, flushMs: Date.now() - flushAt, mark };
}

// In the sleeping simulator a button is what the power button is on the device.
export async function wake(d, { until: stopAt = /Entering activity: (EpubReader|BorgesResume)/, timeoutMs = 45000 } = {}) {
  const boots = d.helloCount;
  const mark = d.mark();
  await d.press("CONFIRM");
  await d.waitBoot(boots, 20000);
  const match = await d.waitLog(stopAt, { since: mark, timeoutMs });
  await d.waitStable(800);
  return { reached: match[1], mark, acts: activities(d.logSince(mark)), boots: d.helloCount - boots };
}

// The throttle says a book checked moments ago is not checked again for two minutes. The
// wall clock of a test does not have two minutes per step, so this is that time passing:
// its record goes, with the preferences reverting to their defaults, which are what these
// tests run with anyway.
export function letThrottleExpire(d) {
  for (const suffix of [".a", ".b"]) fs.rmSync(d.file(`/.crosspoint/borges/glue${suffix}`), { force: true });
}

export function progressOf(d, B) {
  const files = d.progressFiles();
  if (files.length === 0) return null;
  const newest = files.map((f) => ({ f, at: fs.statSync(f).mtimeMs })).sort((a, b) => b.at - a.at)[0].f;
  return B.decodeProgress(fs.readFileSync(newest));
}
