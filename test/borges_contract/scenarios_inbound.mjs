// Highlights made in KOReader or on the web, as the real server delivers them to the firmware,
// and what its list then says. The rows the server stores are not the point here: the wire is.

import { book, bookArgs, bookInfo, xp } from "./scenarios.mjs";
import { KoreaderSim } from "./koreader.mjs";

const listed = async (fw, b, extra = {}) => (await fw.once("inbound", { ...bookArgs(b), ...extra })).highlights;
const texts = (rows) => rows.map((r) => r.text);

async function inbound(h, t) {
  const { account, fw, kr } = await h.fwAndKoreader("lista");
  const a = book(fw, { title: "Con subrayados ajenos" });
  const none = book(fw, { title: "Sin subrayados" });

  // Created in KOReader: everything a row shows arrives.
  const syncA = KoreaderSim.annotationSyncId(a.md5, { kind: "highlight", xpointer: xp(100), pos1: xp(101) });
  const madeAt = 1790000001000;
  kr.enqueueAnnotation({ book: a.md5, bookInfo: bookInfo(a), text: "Desde KOReader ñ 📖", note: "una nota", chapter: "Capítulo 9", page: 60, xpointer: xp(100), pos1: xp(101), timeMs: madeAt });
  kr.enqueueAnnotation({ book: a.md5, bookInfo: bookInfo(a), text: "Antes en el libro", chapter: "Capítulo 2", page: 20, xpointer: xp(50), pos1: xp(51) });
  t.ok((await kr.syncAll()).acks.every((x) => x.status === "accepted"), "KOReader's highlights accepted");
  t.eq((await fw.once("status")).depth, 0, "the firmware has nothing of its own to send");
  t.eq((await listed(fw, a)).length, 0, "before a session, nothing");
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "the firmware pulls");
  let rows = await listed(fw, a);
  t.eq(texts(rows), ["Antes en el libro", "Desde KOReader ñ 📖"], "listed in book order, by the page KOReader sent");
  const one = rows[1];
  t.eq(one.sync_id, syncA, "with KOReader's sync id");
  t.eq(one.chapter, "Capítulo 9", "its chapter");
  t.eq(one.device, "Kobo Libra", "and where it comes from");
  t.eq(one.xpointer, xp(100), "the xpointer to go to");
  t.eq(one.time, Math.floor(madeAt / 1000), "when it was made");
  t.eq(one.has_pct, a.total > 0, "a percentage from page and total");
  t.near(one.pct_micro, Math.floor((60 * 100000000) / a.total), 1, "out of KOReader's own page numbers");
  t.eq((await fw.once("inbound", bookArgs(none))).has_any, false, "another book has none: the drawer row stays away");
  t.eq((await fw.once("inbound", bookArgs(a))).has_any, true, "this one has");

  // A session with nothing new adds nothing. Replays are in the core's tests, with the same log.
  const logs = () => fw.cardFiles().filter((f) => f.includes("inbound") && !f.includes("index"));
  const sizeBefore = fw.fileSize(logs()[0]);
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "another session");
  t.eq((await listed(fw, a)).length, 2, "still two");
  t.eq(fw.fileSize(logs()[0]), sizeBefore, "and not one byte more on the card");

  // Updated: the same row, the new text, the new revision.
  kr.enqueueAnnotation({ book: a.md5, bookInfo: bookInfo(a), text: "Desde KOReader, corregido", chapter: "Capítulo 9", page: 60, xpointer: xp(100), pos1: xp(101), syncId: syncA });
  t.eq((await kr.syncAll()).acks[0].status, "accepted", "the edit is accepted");
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "pulled");
  rows = await listed(fw, a);
  t.eq(texts(rows), ["Antes en el libro", "Desde KOReader, corregido"], "replaced in place");
  t.ok(rows[1].revision >= 2, `with a newer revision: ${rows[1].revision}`);

  // Edited on the web, which is the one writer that is not a reader.
  const [doc] = await h.sql`SELECT id FROM documents WHERE user_id = ${account.id} AND book_hash = ${a.md5}`;
  const [entry] = await h.sql`SELECT id FROM entries WHERE document_id = ${doc.id} AND xpointer = ${xp(50)}`;
  const edit = await h.web("PATCH", `/api/highlights/notes/${entry.id}`, { cookie: account.cookie, body: { note: "Desde la web" } });
  t.eq(edit.status, 200, `the web edits it: ${JSON.stringify(edit.body)}`);
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "the firmware pulls the web's edit");
  rows = await listed(fw, a);
  t.eq(rows.length, 2, "no third row: it is the same highlight");
  t.ok(rows[0].revision >= 2, `newer revision after the web's edit: ${rows[0].revision}`);
  t.note(`the web's edit shows device ${JSON.stringify(rows[0].device)} for a highlight KOReader made`);

  // A highlight that only exists on the web: an imported entry of a book the server knows the hash
  // of, which starts travelling once it is given a note. annotation.created, from the web.
  const b = book(fw, { title: "Sólo en la web" });
  kr.enqueueAnnotation({ book: b.md5, bookInfo: bookInfo(b), text: "El que crea el documento", chapter: "Uno", page: 1, xpointer: xp(1), pos1: xp(2) });
  t.eq((await kr.syncAll()).pending, 0, "the book gets a document");
  const [docB] = await h.sql`SELECT id FROM documents WHERE user_id = ${account.id} AND book_hash = ${b.md5}`;
  const webEntry = await h.sql`
    INSERT INTO entries (document_id, user_id, page, text, chapter, xpointer, drawer)
    VALUES (${docB.id}, ${account.id}, 5, 'Hecho en la web', 'Web', ${xp(5)}, 'lighten') RETURNING id`;
  const madeOnWeb = await h.web("PATCH", `/api/highlights/notes/${webEntry[0].id}`, { cookie: account.cookie, body: { note: "nota" } });
  t.eq(madeOnWeb.status, 200, `a note on it makes it travel: ${JSON.stringify(madeOnWeb.body)}`);
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "the firmware pulls");
  const webRow = (await listed(fw, b)).find((r) => r.text === "Hecho en la web");
  t.ok(webRow, "it is on the list");
  t.eq(webRow?.xpointer, xp(5), "with its place");
  t.eq(webRow?.device, "Borges Web", "and it says where it comes from");

  // Deleted in KOReader: off the list, and off the card.
  kr.enqueueAnnotationDelete({ book: a.md5, bookInfo: bookInfo(a), syncId: syncA });
  t.eq((await kr.syncAll()).acks[0].status, "accepted", "the delete is accepted");
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "pulled");
  rows = await listed(fw, a);
  t.eq(texts(rows), ["Antes en el libro"], "gone from the list");
  t.eq(fw.cardHolds("corregido").filter((f) => !f.endsWith("resp.json")), [], "and its words from the card");

  // A bookmark is not a highlight, and this list says highlights.
  kr.enqueueAnnotation({ book: a.md5, bookInfo: bookInfo(a), kind: "bookmark", text: "Bookmark - page 3", page: 3, xpointer: xp(3) });
  t.eq((await kr.syncAll()).acks[0].status, "accepted", "the bookmark is accepted");
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "pulled");
  t.eq(texts(await listed(fw, a)), ["Antes en el libro"], "and left out");

  // A long one is cut, on a character, and says so.
  kr.enqueueAnnotation({ book: a.md5, bookInfo: bookInfo(a), text: "ñ".repeat(3000), page: 70, xpointer: xp(800), pos1: xp(801) });
  await kr.syncAll();
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "a long text is no problem");
  const long = (await listed(fw, a)).find((r) => r.text.startsWith("ñ"));
  t.eq(long?.truncated, true, "flagged");
  t.ok(long && Buffer.byteLength(long.text) < 480 && !long.text.includes("�"), "kept short, without cutting a character");

  // This reader's own never come back as somebody else's.
  const own = book(fw, { title: "Lo mío" });
  const clips = [{ spine: 0, offset: 7, hash: 7, text: "Subrayado de este lector", chapter: "Uno", xpointer: xp(7), xpointerEnd: xp(8), pctMicro: 7000000 }];
  await fw.once("finalize-book", { ...bookArgs(own), paged: 0, clips: fw.writeClips(clips) });
  t.eq((await fw.once("sleep-session")).report.events_rejected, 0, "the firmware publishes its own");
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "and its echo comes back in a pull");
  t.eq((await listed(fw, own)).length, 0, "which lists nothing");
  await kr.syncAll();

  // A web edit of this reader's own highlight stays this reader's.
  const [docOwn] = await h.sql`SELECT id FROM documents WHERE user_id = ${account.id} AND book_hash = ${own.md5}`;
  const [ownEntry] = await h.sql`SELECT id FROM entries WHERE document_id = ${docOwn.id}`;
  t.eq((await h.web("PATCH", `/api/highlights/notes/${ownEntry.id}`, { cookie: account.cookie, body: { note: "En la web" } })).status, 200, "the web edits it");
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "pulled");
  t.eq((await listed(fw, own)).length, 0, "still not somebody else's");

  // The same passage on both sides. The sync ids differ, as they are derived differently.
  const same = book(fw, { title: "Lo mismo" });
  const passage = "El mismo pasaje";
  kr.enqueueAnnotation({ book: same.md5, bookInfo: bookInfo(same), text: passage, chapter: "Uno", page: 5, xpointer: "/body/DocFragment[3]/body/p[7]/text().0", pos1: "/body/DocFragment[3]/body/p[7]/text().15" });
  kr.enqueueAnnotation({ book: same.md5, bookInfo: bookInfo(same), text: "Otro pasaje", chapter: "Uno", page: 6, xpointer: "/body/DocFragment[3]/body/p[8]/text().0", pos1: "/body/DocFragment[3]/body/p[8]/text().11" });
  t.ok((await kr.syncAll()).acks.every((x) => x.status === "accepted"), "KOReader has it");
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "the firmware pulls");
  t.eq(texts(await listed(fw, same)), [passage, "Otro pasaje"], "with nothing of its own, both show");
  const mine = fw.writeClips([{ spine: 2, offset: 70, hash: 70, text: passage, chapter: "Uno", xpointer: "/body/DocFragment[3]/body/p[7]/text().0", xpointerEnd: "/body/DocFragment[3]/body/p[7]/text().15", pctMicro: 5000000 }], "mine.tsv");
  t.eq(texts(await listed(fw, same, { clips: mine })), ["Otro pasaje"], "with it here as well, once: this reader's own");
  const elsewhere = fw.writeClips([{ spine: 9, offset: 70, hash: 70, text: passage }], "elsewhere.tsv");
  t.eq(texts(await listed(fw, same, { clips: elsewhere })).length, 2, "the same words in another chapter are another passage");

  // A second reader with the same highlight: same sync id, one row, whoever said it last.
  const kr2 = await h.koreader(account, { name: "Kobo Clara" });
  kr2.enqueueAnnotation({ book: same.md5, bookInfo: bookInfo(same), text: passage, chapter: "Uno", page: 5, xpointer: "/body/DocFragment[3]/body/p[7]/text().0", pos1: "/body/DocFragment[3]/body/p[7]/text().15" });
  await kr2.syncAll();
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "the firmware pulls");
  const sameRows = await listed(fw, same);
  t.eq(sameRows.filter((r) => r.text === passage).length, 1, "two KOReaders, one row");

  // Another account: nothing of this one, on the list or on the card.
  await fw.once("forget-credential");
  const other = await h.account("otra");
  await h.pairFirmware(fw, other);
  t.eq((await fw.once("inbound", bookArgs(a))).has_any, false, "another account lists nothing of the first");
  t.eq(fw.cardHolds("Antes en el libro").filter((f) => !f.endsWith("resp.json")), [], "and its words left the card");
  t.eq((await fw.once("manual-session")).report.outcome, "ok", "and it syncs");
  t.eq((await fw.once("inbound", bookArgs(a))).has_any, false, "without any of them coming back");
}

// The bound against the real server, through the real pull: a heavy book, in pages, over sessions.
async function bounded(h, t, metrics) {
  const { fw, kr } = await h.fwAndKoreader("cota");
  const b = book(fw, { title: "Muy subrayado" });
  const total = 300;
  for (let i = 0; i < total; i += 1) {
    kr.enqueueAnnotation({ book: b.md5, bookInfo: bookInfo(b), text: `Subrayado ${i}: ${"palabra ".repeat(20)}`, chapter: "Uno", page: i % 100, xpointer: xp(i + 1000), pos1: xp(i + 1001) });
    if (i % 90 === 89) {
      h.proxy.freshIp();
      await kr.syncAll();
    }
  }
  h.proxy.freshIp();
  t.eq((await kr.syncAll()).pending, 0, "all on the server");
  for (let i = 0; i < 20; i += 1) {
    const { report } = await fw.once("manual-session");
    if (report.events_applied === 0) break;
  }
  const result = await fw.once("inbound", bookArgs(b));
  t.ok(result.highlights.length <= 256, `at most its bound: ${result.highlights.length}`);
  t.ok(result.highlights.length >= 150, "and plenty of them");
  t.eq(result.partial, true, "the list knows it is not all of them");
  t.ok(result.highlights.some((r) => r.text.startsWith("Subrayado 299:")), "the newest are there");
  t.ok(!result.highlights.some((r) => r.text.startsWith("Subrayado 0:")), "the oldest went");
  const logs = fw.cardFiles().filter((f) => f.includes("inbound") && !f.includes("index"));
  t.eq(logs.length, 1, "one file");
  const size = fw.fileSize(logs[0]);
  t.ok(size <= 64 * 1024, `within 64 KiB: ${size}`);
  metrics[`Inbound: card bytes for ${total} highlights of one book`] = size;
}

export const INBOUND = [
  { id: 17, name: "Highlights from KOReader and the web on the firmware's list", run: inbound },
  { id: 18, name: "A heavily highlighted book stays within its bounds", run: bounded },
];
