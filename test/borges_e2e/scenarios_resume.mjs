// Scenario 19: "Go there" with the heap the X4 really has right after the radio.
//
// Found on the first real X4 (7 Nov 2026): the reader opened at the start of the chapter KOReader
// named, ten pages before the place. Two things lined up. KOReader's percentage counts laid out
// pages, so in a book with pictures it sits before the chapter that the xpointer names when
// measured in bytes, as the firmware measures it (77.1 % against a chapter at 78.4-85.1 %). And the
// prompt mapped the xpointer with no framebuffer loan, so inflate asked the heap for its 32 KB
// window, which a C3 just off Wi-Fi does not have. The xpointer went unresolved, the percentage
// was clamped into the chapter, and that is its first page. The host always has the block, so
// BORGES_SIM_TIGHT_HEAP takes it away.

import { bookFixture, koreaderAt, deviceWithBook } from "./scenarios.mjs";
import * as B from "./books.mjs";
import * as ui from "./ui.mjs";

const CHAPTER = 7; // DocFragment[7]: spine 6, from 60 % to 70 % of the book in bytes.
const PARAGRAPH = 30; // Of 40: three quarters into the chapter, pages past its first.

async function scenario19({ h, t, makeDevice }) {
  // Compressed, as every real EPUB is: a stored one is read without inflate and hides the miss.
  const compressed = bookFixture("Ficciones comprimidas", "Jorge Luis Borges", "comprimidas.epub", 1500000, { deflate: true });
  const { account, book, d } = await deviceWithBook(h, makeDevice, "x4", {
    book: compressed,
    device: { env: { BORGES_SIM_TIGHT_HEAP: "1" } },
  });

  // 55 %: before the chapter in bytes, as KOReader's page count puts it in a book with pictures.
  await koreaderAt(h, account, book, { percent: 55, chapter: CHAPTER, paragraph: PARAGRAPH });
  h.proxy.clear();

  const opened = await ui.openFromHome(d);
  t.eq(opened.reached, "BorgesResume", "the open stops at the prompt");
  await d.shot("prompt");
  await ui.choose(d);
  await d.shot("after-go");

  const jumped = ui.progressOf(d, B);
  t.note(`progress.bin after Go: ${JSON.stringify(jumped)}`);
  t.eq(jumped?.spine, CHAPTER - 1, "the reader lands in the chapter the xpointer names");
  t.ok(jumped?.visibleTextOffset > 0, "placed by the xpointer's text, not by the percentage");
  t.eq(/No admitted storage for inflate window/.test(d.logSince(opened.mark)), false, "inflate found its window");

  // Out of the reader, which writes the page it laid out for that text.
  await ui.readerToHome(d);
  const settled = ui.progressOf(d, B);
  t.note(`progress.bin after the reader: ${JSON.stringify(settled)}`);
  t.eq(settled?.spine, CHAPTER - 1, "still that chapter");
  t.ok(settled?.page > 0, `not its first page (page ${settled?.page} of ${settled?.pageCount})`);
  if (settled?.pageCount > 1) {
    const into = settled.page / (settled.pageCount - 1);
    t.near(into, (PARAGRAPH - 1) / 40, 0.12, "about where paragraph 30 of 40 is");
  }
}

export const RESUME_SCENARIOS = [{ id: 19, name: "Go there on the heap of a real X4", run: scenario19 }];
