// Scenarios 16 and 17: highlights made in KOReader, underlined in the page, with the real firmware
// against the real server. The screenshots go to D:\CrossInkBorges\artifacts\e2e\s16: a person looks,
// and the checks here only say what a pixel can (is there a line, how many) and what the log can.

import fs from "node:fs";
import * as B from "./books.mjs";
import { until } from "./device.mjs";
import { bookFixture, deviceWithBook } from "./scenarios.mjs";
import * as ui from "./ui.mjs";

const FILLER =
  "Las palabras siguen y siguen por el corredor de la biblioteca, hexagono tras hexagono, para que cada pagina tenga texto de verdad y el lector tenga que pasarla entera.";
const paragraphOf = (chapter, k) => `Capitulo ${chapter}, parrafo ${k}. ${FILLER}`;

const setSettings = (d, changes) => {
  const file = d.file("/.crosspoint/crossink-settings.json");
  const now = fs.existsSync(file) ? JSON.parse(fs.readFileSync(file, "utf8")) : {};
  fs.writeFileSync(file, JSON.stringify({ ...now, ...changes }));
};

// A phrase of one paragraph, as KOReader writes it: where it starts, where it ends, what it reads.
function phrase(book, chapter, k, words, extra = {}) {
  const text = paragraphOf(chapter, k);
  const at = text.indexOf(words);
  if (at < 0) throw new Error(`not in the paragraph: ${words}`);
  return {
    book: book.md5,
    bookInfo: { title: book.title, author: book.author, file: book.fileName, total_pages: 1000 },
    xpointer: B.xpointerOf(chapter, k, at),
    pos1: B.xpointerOf(chapter, k, at + words.length),
    text: words,
    chapter: `Capitulo ${chapter}`,
    page: chapter * 100 + k,
    timeMs: Date.now() - 3600 * 1000,
    ...extra,
  };
}

// From the middle of one paragraph to the middle of the next. Crengine reads a new line in between.
function acrossParagraphs(book, chapter, k) {
  const first = paragraphOf(chapter, k);
  const start = first.indexOf("para que cada");
  const tail = first.slice(start);
  const second = `Capitulo ${chapter}, parrafo ${k + 1}. Las palabras siguen`;
  return {
    book: book.md5,
    bookInfo: { title: book.title, author: book.author, file: book.fileName, total_pages: 1000 },
    xpointer: B.xpointerOf(chapter, k, start),
    pos1: B.xpointerOf(chapter, k + 1, second.length),
    text: `${tail}\n${second}`,
    chapter: `Capitulo ${chapter}`,
    page: chapter * 100 + k,
    timeMs: Date.now() - 2 * 3600 * 1000,
  };
}

const underlineFiles = (d) => d.allFiles().filter((f) => /[\\/]ul[bp]\d*\.bin$/.test(f));

// Long solid runs in the frame, which nothing else on these pages draws: text has none this wide.
async function linesOnScreen(d, minimum = 28) {
  const pixels = await d.pixels();
  return pixels.solidRuns(minimum);
}

async function openAndSettle(d) {
  const mark = d.mark();
  const opened = await ui.openFromHome(d);
  await d.waitStable(1200);
  return { mark, reached: opened.reached };
}

// A device that once slept in a book wakes into it: what is wanted next is Home.
async function bootAndGoHome(d) {
  const boots = d.helloCount;
  const mark = d.mark();
  await d.start();
  await d.waitBoot(boots);
  const seen = await d.waitLog(/Entering activity: (Home|EpubReader)/, { since: mark });
  await d.waitStable(600);
  if (seen[1] === "EpubReader") await ui.readerToHome(d);
}

async function restartWith(d, changes) {
  await d.kill();
  setSettings(d, changes);
  await bootAndGoHome(d);
}

async function syncFromHome(h, d) {
  await ui.syncNowFromHome(h, d);
  await ui.statusToHome(d);
}

const FLAGS = { hasInbound: true };

// To the first highlight of the list, the way a reader would get there the first time.
async function jumpToFirstHighlight(d, flags = FLAGS) {
  await ui.openLocationTab(d);
  await ui.drawerRow(d, ui.locationRows(flags).inbound);
  let mark = d.mark();
  await d.press("CONFIRM");
  await d.waitLog(/Entering activity: BorgesInbound/, { since: mark });
  await d.waitStable(500);
  mark = d.mark();
  await d.press("CONFIRM");
  await d.waitLog(/Entering activity: EpubReader/, { since: mark, timeoutMs: 40000 });
  await d.waitStable(1500);
  return mark;
}

async function nextPage(d) {
  await d.tap("RIGHT", { settleMs: 1200 });
}

async function scenario16({ h, t, makeDevice }) {
  const { account, book, d } = await deviceWithBook(h, makeDevice, "x4", { prefix: "renglones" });
  const kr = await h.koreader(account, { name: "Kobo Libra" });

  // A book with no highlights asks nothing, keeps nothing and draws nothing.
  ui.letThrottleExpire(d);
  let open = await openAndSettle(d);
  t.eq(open.reached, "EpubReader", "the book opens");
  t.ok(!/Underlines for this book/.test(d.logSince(open.mark)), "no card with highlights: no question about this book");
  t.eq(underlineFiles(d).length, 0, "and nothing on the card");
  t.eq((await linesOnScreen(d)).length, 0, "no line on the page");
  await d.shot("en-no-highlights");
  await ui.readerToHome(d);

  const valid = [
    phrase(book, 2, 1, "biblioteca"), // Leaves the comma out: only the word.
    phrase(book, 2, 2, "el corredor de la biblioteca, hexagono tras hexagono, para que cada pagina tenga texto de verdad"),
    phrase(book, 2, 3, "Las"),
  ];
  const spanning = [8, 12, 16, 20].map((k) => acrossParagraphs(book, 2, k));
  const notThere = phrase(book, 2, 4, "hexagono tras hexagono", { text: "palabras que este libro no tiene" });
  const noSuchPlace = phrase(book, 2, 5, "hexagono tras hexagono", { xpointer: B.xpointerOf(2, 999, 3) });
  for (const item of [...valid, ...spanning, notThere, noSuchPlace]) kr.enqueueAnnotation(item);
  const pushed = await kr.syncAll();
  t.ok(pushed.acks.every((a) => a.status === "accepted"), "KOReader's highlights are accepted");

  ui.letThrottleExpire(d);
  await syncFromHome(h, d);

  // The first open keeps which chapters have one. This chapter has none: no walk.
  open = await openAndSettle(d);
  t.ok(/Underlines for this book: 1/.test(d.logSince(open.mark)), "this book has underlines");
  t.ok(!/Underline plan/.test(d.logSince(open.mark)), "a chapter without any is not walked");
  t.eq((await linesOnScreen(d)).length, 0, "chapter 1 still has no line");
  t.eq(underlineFiles(d).filter((f) => /ulb\.bin$/.test(f)).length, 1, "the list of chapters is on the card");

  // Into chapter 2, in the list's order: the first one is its first paragraph.
  let mark = await jumpToFirstHighlight(d);
  await until("the chapter walked", () => /Underline plan spine=1 /.test(d.logSince(mark)), 40000);
  const planned = d.logSince(mark).match(/Underline plan spine=1 targets=(\d+) verified=(\d+) refused=(\d+) segments=(\d+)/);
  t.eq(Number(planned[1]), valid.length + spanning.length + 2, "every highlight of the chapter is asked about");
  t.eq(Number(planned[2]), valid.length + spanning.length, "and the ones whose text is there are the ones that count");
  await d.waitStable(1500);
  const first = await linesOnScreen(d);
  t.ok(first.length >= 3, `the first page has its lines: ${first.length}`);
  const shotFirst = await d.shot("en-portrait-first-page");
  t.ok(fs.existsSync(shotFirst), "first page on screen");

  // The rest of the chapter's highlights, on their pages. A file read per page, no walk.
  mark = d.mark();
  for (let i = 0; i < 4; i += 1) {
    await nextPage(d);
    await d.shot(`en-portrait-next-${i + 1}`);
  }
  t.ok(!/Underline plan/.test(d.logSince(mark)), "a page turn does not walk anything");
  await d.tap("LEFT", { settleMs: 1200 });
  await d.shot("en-portrait-back-one");
  await ui.readerToHome(d);

  // Closed and opened again with nothing changed: the plan on the card is still true.
  open = await openAndSettle(d);
  t.ok(!/Underline plan/.test(d.logSince(open.mark)), "the same layout and log are not walked again");
  await d.shot("en-portrait-reopened");
  await ui.readerToHome(d);

  // Another orientation is another layout: walked once, while it lays the chapter out anyway.
  await restartWith(d, { language: "ES", orientation: 1 });
  open = await openAndSettle(d);
  t.eq(open.reached, "EpubReader", "opens across");
  await d.waitStable(1500);
  await until("the landscape walk", () => /Underline plan spine=1 /.test(d.logSince(open.mark)), 40000);
  t.ok((await linesOnScreen(d)).length >= 1, "lines across too");
  await d.shot("es-landscape-page");
  await nextPage(d);
  await d.shot("es-landscape-next");
  await nextPage(d);
  await d.shot("es-landscape-next-2");
  await ui.readerToHome(d);

  // Spanish, portrait, a bigger font: the same highlights fall elsewhere, on the right words.
  await restartWith(d, { language: "ES", orientation: 0, fontSize: 3, hyphenationEnabled: 1 });
  open = await openAndSettle(d);
  await d.waitStable(1500);
  await until("the bigger font's walk", () => /Underline plan spine=1 /.test(d.logSince(open.mark)), 40000);
  await d.shot("es-portrait-big-font");
  t.ok((await linesOnScreen(d)).length >= 1, "lines with a bigger font");
  await nextPage(d);
  await d.shot("es-portrait-big-font-next");
  await ui.readerToHome(d);

  // No anti-aliasing: the other path a frame takes to the panel.
  await restartWith(d, { language: "ES", orientation: 0, fontSize: 1, hyphenationEnabled: 0, textAntiAliasing: 0 });
  open = await openAndSettle(d);
  await d.waitStable(1500);
  t.ok((await linesOnScreen(d)).length >= 1, "lines without anti-aliasing");
  await d.shot("es-portrait-no-antialiasing");
  await ui.readerToHome(d);

  // Night mode inverts the whole frame at the panel: the line goes with the text.
  await restartWith(d, { language: "ES", orientation: 0, fontSize: 1, textAntiAliasing: 1, screenInverted: 1 });
  open = await openAndSettle(d);
  await d.waitStable(1500);
  await d.shot("es-night-mode");
  const lightLines = (await d.pixels()).solidRuns(28, true).filter((r) => r.x1 - r.x0 < 400 && r.y > 10);
  t.ok(lightLines.length >= 1, `night mode: light lines under light text, ${lightLines.length}`);
  await ui.readerToHome(d);

  // The row in the reader settings, in both languages, and what it does.
  for (const [language, name] of [["ES", "es"], ["EN", "en"]]) {
    await restartWith(d, { language, orientation: 0, fontSize: 1, screenInverted: 0, inboundUnderlines: 1 });
    t.ok(await ui.homeToSettings(d), `settings open (${name})`);
    await d.tap("CONFIRM", { settleMs: 300 }); // From the Display tab to the Reader tab.
    for (let i = 0; i < 5; i += 1) await d.tap("DOWN", { settleMs: 200 }); // Past the tab bar, to the row.
    await d.shot(`${name}-settings-row`);
    await d.tap("CONFIRM", { settleMs: 400 });
    await d.shot(`${name}-settings-row-off`);
    await d.tap("BACK", { settleMs: 500 });
    await d.kill();
    const saved = JSON.parse(fs.readFileSync(d.file("/.crosspoint/crossink-settings.json"), "utf8"));
    t.eq(saved.inboundUnderlines, 0, `the row turned it off (${name})`);
    setSettings(d, { inboundUnderlines: 1 });
    await bootAndGoHome(d);
  }

  // The setting: off draws nothing and walks nothing, and the reader says so in its settings.
  await restartWith(d, { language: "ES", orientation: 0, fontSize: 1, textAntiAliasing: 1, inboundUnderlines: 0 });
  open = await openAndSettle(d);
  await d.waitStable(1500);
  t.ok(!/Underline/.test(d.logSince(open.mark)), "turned off, nothing is asked");
  t.eq((await linesOnScreen(d)).length, 0, "and nothing drawn");
  await d.shot("es-setting-off");
  await ui.readerToHome(d);
  await restartWith(d, { language: "EN", inboundUnderlines: 1 });

  // This reader highlights the same passage KOReader has: it stays the reader's, not underlined.
  open = await openAndSettle(d);
  await d.waitStable(1500);
  await jumpToFirstHighlight(d);
  const before = (await linesOnScreen(d)).length;
  t.ok(before >= 3, `back on the first page, with its lines: ${before}`);
  // As ui.createClipping, but a first press can go to the selection's guard against the press that
  // opened it, with this little time between the two here.
  await ui.openLocationTab(d);
  await ui.drawerRow(d, ui.locationRows(FLAGS).clip);
  const clipMark = d.mark();
  await d.press("CONFIRM");
  await d.waitLog(/Entering activity: \w*Clip\w*/, { since: clipMark });
  await d.waitStable(800);
  const framesBefore = d.frames;
  await d.press("CONFIRM");
  await d.waitStable(600);
  if (d.frames === framesBefore) await d.tap("CONFIRM", { settleMs: 200 });
  for (let i = 0; i < 6; i += 1) await d.tap("RIGHT", { settleMs: 150 });
  await d.tap("CONFIRM", { settleMs: 800 });
  await d.shot("en-own-clipping-made");
  await d.tap("RIGHT", { settleMs: 300 });
  await ui.sleepAndWaitFlush(d);
  const ownRows = await h.sql`SELECT e.text, e.chapter, e.xpointer FROM entries e JOIN documents dd ON dd.id = e.document_id WHERE dd.user_id = ${account.id} AND dd.book_hash = ${book.md5} `;
  t.ok(ownRows.length >= 1, `the clipping reached the server: ${ownRows[0]?.text}`);
  if (ownRows[0]) {
    kr.enqueueAnnotation({
      book: book.md5,
      bookInfo: { title: book.title, author: book.author, file: book.fileName, total_pages: 1000 },
      text: ownRows[0].text,
      chapter: ownRows[0].chapter,
      page: 201,
      xpointer: ownRows[0].xpointer,
      pos1: B.xpointerOf(2, 1, ownRows[0].text.length),
    });
    t.ok((await kr.syncAll()).acks.every((a) => a.status === "accepted"), "KOReader's copy is accepted");
  }
  ui.letThrottleExpire(d);
  await ui.wake(d);
  await ui.readerToHome(d);
  await syncFromHome(h, d);
  ui.letThrottleExpire(d);
  open = await openAndSettle(d);
  await d.waitStable(1500);
  await until("the walk after the clipping", () => /Underline plan spine=1 /.test(d.logSince(open.mark)), 40000);
  const withOwn = d.logSince(open.mark).match(/Underline plan spine=1 targets=(\d+) verified=(\d+)/);
  t.eq(Number(withOwn[2]), valid.length + spanning.length, "KOReader's copy of an own passage is not one more to underline");
  await jumpToFirstHighlight(d, { ...FLAGS, hasClippings: true });
  await d.shot("en-own-clipping-and-underlines");
  t.ok((await linesOnScreen(d)).length >= before - 1, "the others keep their lines");
  await ui.readerToHome(d);

  await d.kill();
}

// The largest a book can be: 256 records, all in one chapter. Everything measured in the same run.
async function scenario17({ h, t, makeDevice }) {
  const fixture = bookFixture("Ficciones medidas", "Jorge Luis Borges", "medidas.epub", 1500000, { paragraphs: 400, chapters: 4 });
  const { account, book, d } = await deviceWithBook(h, makeDevice, "x4", { prefix: "medida", book: fixture });
  const kr = await h.koreader(account, { name: "Kobo Libra" });

  const turnsOf = (text) =>
    [...text.matchAll(/Page composed in (\d+) us/g)].map((m) => Number(m[1])).sort((a, b) => a - b);
  const median = (values) => (values.length ? values[Math.floor(values.length / 2)] : -1);
  const worst = (values) => (values.length ? values[values.length - 1] : -1);
  const turnPages = async (count, label) => {
    const mark = d.mark();
    for (let i = 0; i < count; i += 1) await d.tap("RIGHT", { settleMs: 250 });
    await d.waitStable(600);
    const values = turnsOf(d.logSince(mark));
    t.note(`${label}: ${values.length} pages, composed median ${median(values)} us, worst ${worst(values)} us`);
    return values;
  };

  // In chapter 2 without any highlight anywhere, then with the setting off, then with all of them.
  ui.letThrottleExpire(d);
  await openAndSettle(d);
  await turnPages(20, "no highlights, chapter 1");
  await ui.readerToHome(d);

  // About as long as they can be while 256 still fit under the log's 64 KB.
  const long = "Las palabras siguen y siguen por el corredor de la biblioteca, hexagono tras hexagono, para que cada pagina";
  for (let k = 1; k <= 256; k += 1) kr.enqueueAnnotation(phrase(book, 1, k, long, { page: k }));
  const pushed = await kr.syncAll();
  t.ok(pushed.acks.every((a) => a.status === "accepted"), "256 highlights accepted");
  // A session brings what it has time and room for: as many as it takes.
  for (let i = 0; i < 4; i += 1) {
    ui.letThrottleExpire(d);
    await syncFromHome(h, d);
  }
  const log = d.allFiles().filter((f) => f.includes("inbound") && !f.includes("index"));
  t.eq(log.length, 1, "one log");
  t.note(`log on the card: ${fs.statSync(log[0]).size} bytes`);

  await restartWith(d, { inboundUnderlines: 0 });
  await openAndSettle(d);
  const off = await turnPages(20, "256 on the card, setting off");
  await ui.readerToHome(d);

  await restartWith(d, { inboundUnderlines: 1 });
  const open = await openAndSettle(d);
  await d.waitStable(2000);
  await until("the walk", () => /Underline plan spine=0 /.test(d.logSince(open.mark)), 60000);
  const plan = d.logSince(open.mark).match(/Underline plan spine=0 targets=(\d+) verified=(\d+) refused=(\d+) segments=(\d+) timedOut=(\d) in (\d+) ms free=(\d+)/);
  t.note(`plan: targets ${plan[1]} verified ${plan[2]} refused ${plan[3]} segments ${plan[4]} timedOut ${plan[5]} in ${plan[6]} ms, free after ${plan[7]}`);
  t.eq(Number(plan[2]), 256, "all of them found");
  const on = await turnPages(20, "256 in this chapter, underlined");
  const draws = [...d.logSince(open.mark).matchAll(/Page composed in \d+ us \(underlines (\d+) us\)/g)]
    .map((m) => Number(m[1]))
    .filter((us) => us > 0)
    .sort((a, b) => a - b);
  t.note(`inside that, reading, matching and drawing the lines: ${draws.length} pages, median ${median(draws)} us, worst ${worst(draws)} us`);
  t.note(`added to a composed page, median: ${median(on) - median(off)} us`);
  t.note(`file reads added with underlines: at most 1 open and ${Math.ceil(Math.log2(512)) + 1} small reads per window of 16 segments`);
  await d.shot("256-underlined");
  await ui.readerToHome(d);
  await d.kill();
}

// A walk that kills the reader can do it once, not at every entry to that chapter.
async function scenario18({ h, t, makeDevice }) {
  const { account, book, d } = await deviceWithBook(h, makeDevice, "x4", { prefix: "caida" });
  const kr = await h.koreader(account, { name: "Kobo Libra" });
  const walkMarks = () => d.allFiles().filter((f) => /[\\/]ulw\d+\.bin$/.test(f));
  const push = async (item) => {
    kr.enqueueAnnotation(item);
    t.ok((await kr.syncAll()).acks.every((a) => a.status === "accepted"), "KOReader's highlight is accepted");
    ui.letThrottleExpire(d);
    await syncFromHome(h, d);
  };

  // The book left in chapter 2, whose lines are known.
  await push(phrase(book, 2, 1, "biblioteca"));
  await openAndSettle(d);
  let mark = await jumpToFirstHighlight(d);
  await until("the first walk", () => /Underline plan spine=1 /.test(d.logSince(mark)), 40000);
  t.eq(walkMarks().length, 0, "a walk that came back leaves no mark");
  await ui.readerToHome(d);

  // A new highlight means a new walk, and this one dies.
  await push(phrase(book, 2, 2, "el corredor"));
  await d.kill();
  const boots = d.helloCount;
  await d.start({ env: { BORGES_SIM_DIE_IN_WALK: "1" } });
  await d.waitBoot(boots);
  await d.waitLog(/Entering activity: Home/);
  await d.waitStable(600);
  await d.press("CONFIRM");
  await until("the reader to die in the walk", () => !d.running, 40000);
  t.eq(walkMarks().length, 1, "it died with the mark on the card");
  await d.kill();

  // Back: the same chapter opens, without lines and without a second walk.
  await bootAndGoHome(d);
  let open = await openAndSettle(d);
  t.eq(open.reached, "EpubReader", "the book opens after the crash");
  t.ok(/The last walk of spine 1 did not finish/.test(d.logSince(open.mark)), "it knows what happened");
  t.ok(!/Underline plan/.test(d.logSince(open.mark)), "and does not walk again");
  t.eq(walkMarks().length, 0, "the mark is gone");
  t.eq((await linesOnScreen(d)).length, 0, "that chapter has no lines");
  await d.shot("after-the-crash");
  await ui.readerToHome(d);

  open = await openAndSettle(d);
  t.ok(!/did not finish|Underline plan/.test(d.logSince(open.mark)), "the next entry is quiet");
  await ui.readerToHome(d);

  // Something new on the log: it is tried again, and here it comes back.
  await push(phrase(book, 2, 3, "Las"));
  open = await openAndSettle(d);
  await until("the walk after the log moved", () => /Underline plan spine=1 /.test(d.logSince(open.mark)), 40000);
  await d.waitStable(1500);
  t.ok((await linesOnScreen(d)).length >= 1, "the lines are back");
  await d.shot("lines-back");
  await ui.readerToHome(d);
  await d.kill();
}

export const UNDERLINE_SCENARIOS = [
  { id: 16, name: "Other readers' highlights underlined in the page", run: scenario16 },
  { id: 17, name: "Underlines at the bound: 256 records in one chapter", run: scenario17 },
  { id: 18, name: "A walk that crashes the reader does it once", run: scenario18 },
];
