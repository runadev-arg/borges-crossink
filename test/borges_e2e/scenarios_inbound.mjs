// Scenario 14: highlights made in KOReader, on the real screen. The screenshots go to
// D:\CrossInkBorges\artifacts\e2e\s14, and from there to round2-screens by hand: a person looks.

import fs from "node:fs";
import * as B from "./books.mjs";
import { until } from "./device.mjs";
import { deviceWithBook } from "./scenarios.mjs";
import * as ui from "./ui.mjs";

const setSettings = (d, changes) => {
  const file = d.file("/.crosspoint/crossink-settings.json");
  const now = fs.existsSync(file) ? JSON.parse(fs.readFileSync(file, "utf8")) : {};
  fs.writeFileSync(file, JSON.stringify({ ...now, ...changes }));
};

// Chapter n of a ten-chapter book sits at about (n-1)/10 of it. Long chapter and device names,
// which is what a row has to survive.
function highlights(book) {
  const info = { title: book.title, author: book.author, file: book.fileName, total_pages: 1000 };
  const at = (chapter, paragraph, length) => ({
    book: book.md5,
    bookInfo: info,
    xpointer: B.xpointerOf(chapter, paragraph),
    pos1: B.xpointerOf(chapter, paragraph, length),
  });
  const longText = "Las palabras siguen y siguen por el corredor de la biblioteca, hexagono tras hexagono, para que cada pagina tenga texto de verdad y el lector tenga que pasarla entera, y aqui esta lo que un lector quiso guardar para despues, que es bastante mas de lo que entra en tres renglones de esta pantalla.";
  const now = Date.now();
  return [
    { ...at(2, 3, 20), text: "Capitulo 2, parrafo 3. Un subrayado corto.", chapter: "Capitulo 2", page: 110, timeMs: now - 3 * 3600 * 1000 },
    { ...at(4, 5, 40), text: longText, chapter: "Un capitulo con un nombre larguisimo que no entra en la linea de una pantalla angosta", page: 320, timeMs: now - 2 * 86400 * 1000 },
    { ...at(6, 7, 20), text: "Capitulo 6, parrafo 7. Con acentos: año, canción, último.", chapter: "Capitulo 6", page: 520, timeMs: now - 90 * 1000 },
    { ...at(7, 8, 20), text: "Capitulo 7, parrafo 8. Sin capitulo ni fecha." , page: 610 },
    { ...at(8, 2, 20), text: "Capitulo 8, parrafo 2.", chapter: "Capitulo 8", page: 710, timeMs: now - 40 * 86400 * 1000 },
    { ...at(9, 4, 20), text: "Capitulo 9, parrafo 4.", chapter: "Capitulo 9", page: 810, timeMs: now - 26 * 3600 * 1000 },
    { ...at(10, 6, 20), text: "Capitulo 10, parrafo 6.", chapter: "Capitulo 10", page: 910, timeMs: now - 7 * 60 * 1000 },
    { ...at(10, 9, 20), text: "Capitulo 10, parrafo 9, el ultimo de la lista: la segunda pagina.", chapter: "Capitulo 10", page: 950, timeMs: now - 5 * 60 * 1000 },
  ];
}

// A device that once slept in a book wakes into it, and a power cut comes back the same way even
// after going Home since: either is fine here, what is wanted next is Home.
async function bootAndGoHome(d, opts) {
  const boots = d.helloCount;
  const mark = d.mark();
  await d.start(opts);
  await d.waitBoot(boots);
  const seen = await d.waitLog(/Entering activity: (Home|EpubReader)/, { since: mark });
  await d.waitStable(600);
  if (seen[1] === "EpubReader") await ui.readerToHome(d);
}

const FLAGS = { hasClippings: true, hasInbound: true };

async function syncFromHome(h, d) {
  await ui.syncNowFromHome(h, d);
  await ui.statusToHome(d);
}

async function openInboundList(d, flags, tab = {}) {
  await ui.openLocationTab(d, tab);
  await ui.drawerRow(d, ui.locationRows(flags).inbound);
  await d.shot("drawer-row");
  const mark = d.mark();
  await d.press("CONFIRM");
  await d.waitLog(/Entering activity: BorgesInbound/, { since: mark });
  await d.waitStable(500);
  return mark;
}

// Back from the list is the drawer it was opened from, and Back again the page.
async function listToPage(d) {
  let mark = d.mark();
  await d.press("BACK");
  await d.waitLog(/Entering activity: \w*Drawer/, { since: mark });
  await d.waitStable(300);
  mark = d.mark();
  await d.press("BACK");
  await d.waitLog(/Exiting activity: \w*Drawer/, { since: mark });
  await d.waitStable(600);
}

async function goToFocused(d) {
  const mark = d.mark();
  await d.press("CONFIRM");
  await d.waitLog(/Entering activity: EpubReader/, { since: mark, timeoutMs: 40000 });
  await d.waitStable(800);
}

async function scenario14({ h, t, makeDevice }) {
  const { account, book, d } = await deviceWithBook(h, makeDevice, "x4", { prefix: "ajenos" });
  const kr = await h.koreader(account, { name: "Kobo Libra de la casa de campo" });

  // Before: no row, and the drawer's rows are where they always were.
  await ui.openFromHome(d);
  await ui.openLocationTab(d);
  await d.shot("drawer-without-highlights");
  await d.tap("BACK", { settleMs: 300 });

  // A clipping made here, and KOReader with that very passage. Their sync ids differ: only the
  // words can tell. Its echo and KOReader's copy are not somebody else's highlight.
  await d.tap("RIGHT", { settleMs: 200 });
  await ui.createClipping(d, {}, 6);
  await d.shot("own-clipping");
  await d.tap("RIGHT", { settleMs: 300 });
  await ui.sleepAndWaitFlush(d);
  const [own] = await h.sql`SELECT e.text, e.chapter, e.xpointer FROM entries e JOIN documents dd ON dd.id = e.document_id WHERE dd.user_id = ${account.id} AND dd.book_hash = ${book.md5}`;
  t.ok(own?.text?.length > 10, `the clipping reached the server: ${own?.text}`);
  const ownPassage = { book: book.md5, bookInfo: { title: book.title, author: book.author, file: book.fileName, total_pages: 1000 }, text: own.text, chapter: own.chapter, page: 20, xpointer: own.xpointer, pos1: B.xpointerOf(1, 9, 3) };
  kr.enqueueAnnotation(ownPassage);
  t.ok((await kr.syncAll()).acks.every((a) => a.status === "accepted"), "KOReader's copy is accepted");
  ui.letThrottleExpire(d);
  await ui.wake(d);
  await ui.readerToHome(d);
  await syncFromHome(h, d);
  t.eq(d.allFiles().filter((f) => f.includes("inbound") && !f.includes("index")).length, 1, "KOReader's copy is on the card");
  ui.letThrottleExpire(d);
  const ownOpen = d.mark();
  await ui.openFromHome(d);
  await ui.openLocationTab(d);
  await d.shot("drawer-only-own-passage");
  await d.waitLog(/Highlights of other readers in this book: 0/, { since: ownOpen, timeoutMs: 5000 });
  await d.tap("BACK", { settleMs: 300 });
  await ui.readerToHome(d);

  for (const item of highlights(book)) kr.enqueueAnnotation(item);
  const pushed = await kr.syncAll();
  t.ok(pushed.acks.every((a) => a.status === "accepted"), "KOReader's highlights are accepted");
  t.eq(pushed.pending, 0, "and none are left");

  ui.letThrottleExpire(d);
  await syncFromHome(h, d);
  t.eq(d.allFiles().filter((f) => f.includes("inbound") && !f.includes("index")).length, 1, "one log, for this book");

  // English first. The list in book order, with the focus on the first.
  ui.letThrottleExpire(d);
  const opened = await ui.openFromHome(d);
  t.eq(opened.reached, "EpubReader", "the book opens");
  await d.tap("RIGHT", { settleMs: 200 });
  await openInboundList(d, FLAGS);
  const listEn = await d.shot("en-list");
  t.ok(fs.existsSync(listEn), "the list is on screen");
  await d.tap("DOWN", { settleMs: 300 });
  await d.shot("en-list-second-focused");
  await d.tap("DOWN", { settleMs: 200 });
  await d.tap("DOWN", { settleMs: 200 });
  await d.tap("DOWN", { settleMs: 200 });
  await d.tap("DOWN", { settleMs: 200 });
  await d.tap("DOWN", { settleMs: 500 });
  await d.shot("en-list-page-two");
  // Back to the first row, the third highlight's neighbour: goes up the page.
  for (let i = 0; i < 6; i += 1) await d.tap("UP", { settleMs: 150 });

  // Confirm: the highlight's chapter, with the way back in the menu.
  const before = ui.progressOf(d, B);
  t.eq(before.spine, 0, "still in the first chapter");
  await goToFocused(d);
  await d.shot("en-jumped");
  const jumped = ui.progressOf(d, B);
  t.eq(jumped.spine, 1, `the highlight's chapter: ${JSON.stringify(jumped)}`);
  t.ok(jumped.visibleTextOffset !== undefined, "an exact place, from its xpointer");
  // Anything else in the book counts as having been read here.
  await ui.openLocationTab(d);
  await d.shot("en-drawer-with-undo");
  await ui.drawerRow(d, ui.locationRows({ ...FLAGS, hasUndo: true }).undo);
  await d.shot("en-drawer-undo-focused");
  const undoMark = d.mark();
  await d.press("CONFIRM");
  await d.waitLog(/Exiting activity: EpubReader/, { since: undoMark, timeoutMs: 20000 });
  await d.waitLog(/Entering activity: EpubReader/, { since: undoMark, timeoutMs: 40000 });
  await d.waitStable(800);
  await d.shot("en-back-where-it-was");
  const back = ui.progressOf(d, B);
  t.eq(back.spine, 0, `the way back: the first chapter again: ${JSON.stringify(back)}`);
  await ui.readerToHome(d);

  // A place that cannot be found: only the percentage, from the page numbers. The server takes it,
  // since it has a page, and KOReader's position map is no help here.
  kr.enqueueAnnotation({ book: book.md5, bookInfo: { title: book.title, author: book.author, file: book.fileName, total_pages: 1000 }, syncId: "bbbbbbbb-0000-3000-8000-000000000001", text: "Sin lugar exacto: la mitad del libro.", chapter: "Quien sabe", page: 500, xpointer: "/body/DocFragment[6]/body/nada[9]/text().0", pos1: "/body/DocFragment[6]/body/nada[9]/text().9" });
  // And no place at all: not even a total to make a percentage out of.
  kr.enqueueAnnotation({ book: book.md5, bookInfo: { title: book.title, author: book.author, file: book.fileName }, syncId: "bbbbbbbb-0000-3000-8000-000000000002", text: "Sin ningun lugar donde ir.", page: 12 });
  const more = await kr.syncAll();
  t.ok(more.acks.every((a) => a.status === "accepted"), `accepted: ${JSON.stringify(more.acks.map((a) => a.rejection_code))}`);
  ui.letThrottleExpire(d);
  await syncFromHome(h, d);
  await d.kill();

  // Spanish, the longer of the two.
  setSettings(d, { language: "ES" });
  await bootAndGoHome(d, { env: { BORGES_SIM_HOLD_JUMP_NOTICE: "1" } });
  await ui.openFromHome(d);
  await openInboundList(d, FLAGS);
  await d.shot("es-list");
  await d.tap("DOWN", { settleMs: 300 });
  await d.shot("es-list-focus");
  // Row: Sin lugar exacto sorts with its 50%, between the 32% and the 52%. Then the one with no place.
  await d.tap("DOWN", { settleMs: 300 });
  await d.shot("es-approximate-focused");
  const approxMark = d.mark();
  await d.press("CONFIRM");
  // What the screen says while the page is built, held for the screenshot by the simulator.
  await d.waitLog(/Went to a highlight: spine=5 exact=0/, { since: approxMark, timeoutMs: 20000 });
  await d.shot("es-approximate-while-it-builds");
  await d.waitLog(/Entering activity: EpubReader/, { since: approxMark, timeoutMs: 40000 });
  await d.waitStable(800);
  await d.shot("es-approximate-landed");
  const approx = ui.progressOf(d, B);
  t.ok(approx.spine === 5, `the percentage, in the chapter it names: ${JSON.stringify(approx)}`);

  await openInboundList(d, { ...FLAGS, hasUndo: true });
  await d.tap("UP", { settleMs: 300 });  // Wraps to the last row: the one with nowhere to go.
  await d.shot("es-no-place-focused");
  const framesBefore = d.frames;
  const noPlace = d.mark();
  await d.press("CONFIRM");
  await d.waitNewFrame(framesBefore);
  await until("the toast", async () => /says nowhere to go/.test(d.logSince(noPlace)), 8000);
  await d.waitStable(200);
  await d.shot("es-no-place-toast");
  const toastFrames = d.frames;
  await d.waitNewFrame(toastFrames, 5000);
  await d.waitStable(500);
  await d.shot("es-no-place-after");
  t.ok(!/Exiting activity: EpubReader/.test(d.logSince(noPlace)), "it says so, and the book stays where it was");
  t.eq(ui.progressOf(d, B).spine, approx.spine, "not one page moved");
  await ui.readerToHome(d);
  await d.kill();

  // Landscape: the rows are wider and shorter, and the button hints move.
  setSettings(d, { language: "ES", orientation: 1 });
  await bootAndGoHome(d);
  await ui.openFromHome(d);
  await d.shot("es-landscape-page");
  await openInboundList(d, { ...FLAGS, hasUndo: true }, { portrait: false });
  await d.shot("es-landscape-list");
  await d.tap("DOWN", { settleMs: 300 });
  await d.shot("es-landscape-list-focus");
  await listToPage(d);
  await ui.readerToHome(d);
  await d.kill();

  // The other theme family: Classic, with its own header and hints.
  setSettings(d, { language: "ES", orientation: 0, uiTheme: 0 });
  await bootAndGoHome(d);
  await ui.openFromHome(d);
  await openInboundList(d, { ...FLAGS, hasUndo: true });
  await d.shot("es-classic-list");
  await listToPage(d);
  await ui.readerToHome(d);
  await d.kill();

  // The delete: gone from the list, and when it was the last, so is the row. Eight and two.
  setSettings(d, { language: "EN", orientation: 0, uiTheme: 1 });
  for (const item of [...highlights(book), ownPassage]) kr.enqueueAnnotationDelete({ book: book.md5, bookInfo: item.bookInfo, syncId: kr.constructor.annotationSyncId(book.md5, { kind: "highlight", xpointer: item.xpointer, pos1: item.pos1 }) });
  kr.enqueueAnnotationDelete({ book: book.md5, syncId: "bbbbbbbb-0000-3000-8000-000000000001" });
  kr.enqueueAnnotationDelete({ book: book.md5, syncId: "bbbbbbbb-0000-3000-8000-000000000002" });
  const gone = await kr.syncAll();
  t.ok(gone.acks.every((a) => a.status === "accepted"), `the deletes are accepted: ${JSON.stringify(gone.acks.map((a) => a.rejection_code))}`);
  await bootAndGoHome(d);
  for (let i = 0; i < 2; i += 1) {
    await syncFromHome(h, d);
    ui.letThrottleExpire(d);
  }
  await ui.openFromHome(d);
  await ui.openLocationTab(d);
  await d.shot("en-drawer-after-delete");
  t.eq(d.allFiles().filter((f) => f.includes("inbound") && !f.includes("index")).length, 0, "no log is left on the card");
  await d.tap("BACK", { settleMs: 300 });
  await ui.readerToHome(d);
}

export const INBOUND_SCENARIOS = [{ id: 14, name: "Highlights from other readers on the screen", run: scenario14 }];
