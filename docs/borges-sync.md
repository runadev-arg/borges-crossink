# Borges Sync on CrossInk: design contract

Native client for the Borges hub, written from scratch for the Xteink X4 (ESP32-C3, ~380 KB RAM,
no always-on Wi-Fi budget). It must feel like the KOReader plugin. Server contract:
`highlights-kobo/docs/toto-sync-v2-protocol.md` and `device-login-v1.md`.

## 1. What the reader experiences

| Moment | Behaviour |
|---|---|
| First time | Home shows **Borges**. Pair shows a code and QR to `https://borges.runadev.com/devices/pair?code=XXXX-XXXX`, polls by itself and finishes alone. A credential left on the SD card by the previous Borges firmware is imported silently. |
| Open a book, or wake into one | Before the reader loads: connect to a saved network, ask "where was I?" for this exact edition, push pending work. If another device read further or later: **¿Dónde querés seguir leyendo?** Any button skips and opens now. No network within budget: opens anyway, nothing is lost. |
| Sleep (manual or timeout) | Finalize position, push what is pending within a hard budget, then sleep. Skipped on low battery. |
| Leave a book to Home | Position is finalized locally. It goes out on the next open or sleep. No surprise reboot on Home. |
| Manual | Reader drawer **Sincronizar ahora**, assignable button shortcut, and Home → Borges. |
| Library | Home → Borges → Biblioteca: paged list of the account's books, one-press download with resume and integrity check, marked when already on the SD card. |
| Highlights from elsewhere | Reader drawer **Subrayados de otros lectores**, only in a book that has some: a list to read and to jump from. Section 7. |

Never: Wi-Fi on while reading, an error dialog interrupting reading, a jump without consent.

**When the radio comes on.** Only in a session: on opening a book, on waking into one, before
sleep, and when the person asks (Borges screen, reader drawer, shortcut). Nothing runs while a page
is being read, so connecting to Wi-Fi after a book is open cannot trigger the offer: the next
session is the next open, wake, sleep or manual sync. The open session is capped at 12 s from the
press to the book (7 s of that for the link). A longer link budget was not measured on a panel,
so it stays. The one exception is the first open with no clock anchor or no link hint, which is to
say the first after flashing: 20 s from the boot (14 s for the link), since a scan, a join, finding
the time, DNS and TLS are all still ahead of it and missing the offer there costs the most. With
nothing to send, an open ends after the check: history waits for a session nobody is waiting on.

**When the check could not happen.** No network, a server that did not answer, a low battery, the
crash guard: the book opens anyway, which looks exactly like nobody having read it. So the page says
once `No pude buscar tu posición. Menú > Sincronizar ahora`, until it turns. Not when a person
pressed a button to skip it, not with the search off, and not at every wake away from any network:
after saying it, the next 8 misses on that book go by quietly. No clock is involved in that, since a
wake from deep sleep often has none. A check that gets an answer starts it over.

## 2. Why a network session is bracketed by reboots

A loaded reader leaves ~45-50 KB free. TLS needs ≥35 KB and ~17 KB contiguous per record, on top of
the Wi-Fi stack. CrossInk already reboots into a minimal network boot and reboots out. We reuse that
mechanism, and because the sessions happen at the seams (before the book loads, before sleep) reading
itself never pauses. A wake boot is already clean, so it needs no extra reboot. Target: the prompt is
on screen within ~8 s of pressing the book, on a known network.

## 3. Layers

```
lib/BorgesCore/   Pure C++. No Arduino, no HAL. Host-tested. Depends only on injected interfaces.
  Ports.h           IFs, IHttp, IClock, IRandom, ICancel
  AtomicFile        Two-slot CRC-checked records; a torn write always falls back to the last good one
  Store             Credentials, sync state, per-book ledger
  Inbound           Other devices' highlights: one bounded log per book, and the list that reads it
  Outbox            Append-only CRC log + ack watermark; immutable events; sequence repair
  Events            progress.changed, annotation.*, session.ended envelopes
  Spool/JsonStream  Responses go to SD first and are parsed element by element
  ResumePolicy      The KOReader decision table, pure
  SyncEngine        One session: focus check, pull pages, drain outbox, report
  PairingFlow       request / status / claim state machine
  Catalog           Paged library manifest cached on SD
src/borges/       Device glue: HalStorage IFs, SecureHttpClient IHttp, DNS + clock boot, activities
```

Everything that can be decided without hardware lives in `lib/BorgesCore`.

## 4. Rules that fix what failed before

These are requirements. Each has a regression test named after it.

1. **Spool, never buffer.** Sync responses stream to `/.crosspoint/borges/spool/` and are parsed one
   array element at a time with a filter. The only cap (4 MiB, far above any server page) protects
   the card. A single event larger than the parse budget is skipped with its cursor still advancing,
   and is logged. Never a poison page.
2. **One TLS connection per session**, keep-alive, reconnect once on a dropped socket.
3. **Request bodies are bounded** (default 8 KiB per exchange, always at least one event), max 20
   events per batch. A single oversized event is sent alone. Server cap is 256 KiB.
4. **Outbox cannot be poisoned.** A corrupt record is quarantined to `outbox.bad` and skipped, and
   `depth()` counts exactly what `nextBatch()` can return.
5. **No per-page disk churn.** Page turns touch only CrossInk's `progress.bin`. A progress event is
   materialized when the book is finalized (exit, sleep, session start) with `occurred_at` = last
   real page turn. A book that was not paged produces no event. At most one unattempted progress
   event per book: it may be replaced until its first attempt, then it is immutable.
6. **Honest clock.** `time_precision`: `exact` only when the wall clock came from a `server_time` on
   a verified connection this power cycle or a monotonic chain since then, `anchored` when derived
   from a stored server anchor across deep sleep, `approximate` from SNTP or an unverified Date
   header (neither is authenticated), `unknown` otherwise. Never label stale time `exact`. A repaired
   time must be at or past the anchor and the build date, and within a year of them unless a person
   asked for the session. `settimeofday` is applied from `server_time` only when the clock is invalid
   or worse than the anchor.
7. **Trust is real.** VERIFY_PEER, a checked CA load and hostname verification, with several anchors
   (ISRG Root YE, X1, X2, GTS R1, GTS R4, plus a fallback chain), so a CA change at Cloudflare does
   not end the service. Build flags: `WOLFSSL_SP_384`, `WOLFSSL_ALT_CERT_CHAINS`, `WOLFSSL_SHA384`,
   `WOLFSSL_SHA512`, `HAVE_MAX_FRAGMENT`.
8. **DNS chain**: getaddrinfo → DNS over TCP (1.1.1.1, 8.8.8.8) → cached IP → baked Cloudflare IPs for
   the production host only. A failure never sticks for the boot. Use `lwip_getaddrinfo`, never
   `hostByName` (it calls `dns_clear_cache()` without the TCPIP lock and panics with SNTP in flight).
   Certificate name and SNI stay on the hostname when dialing by IP.
9. **Pull before push.** If pull fails, do not drain: stale progress would become the latest write.
   `has_more` is honoured up to 4 pages per session, and the rest continues next session.
10. **Cursor discipline.** Persist applied state and cursor atomically, then send the new cursor.
    Commit the last page with `cursor/commit`. Respect `cursor_regression` and `cursor_not_delivered`.
11. **Sequence repair** per protocol §2: use `details.next_client_sequence` and `unrecorded_events`,
    persist before a single retry, keep ids and order. Never discard without explicit confirmation.
12. **Failure is diagnosable.** Stage, code and detail persist in the state file and appear on the
    Borges screen in plain words, with the `X-Request-Id`. Tokens never reach logs.
13. **Wi-Fi is always turned off** when a session ends, by every path, including errors and aborts.
14. **Cancellable.** Sessions check `ICancel` between steps and inside streams. A button press during
    the open-book session opens the book at once and loses nothing.
15. **Bounded.** Every session has a wall-clock budget (open 12 s, sleep 15 s, manual 120 s). Backoff
    `15·2^n` s, capped at 6 h, ±20% jitter, applied to automatic sessions only. Manual always tries.
16. **Non-catalog books still work:** progress is keyed by partial MD5 and accepted with a null
    document. Hash read errors produce "no identity", never a wrong hash.

## 5. Resume decision (same as KOReader)

Check: `POST /api/sync/v2/pull` with `focus_book` and `focus_only:true`. While the check is
unresolved, that book's `progress.changed` is held back from upload.

- Ask = false when: no `focus_event`, or the event is from this device, or a stored local choice has
  the same fingerprint. Reading here afterwards does not reopen an answered question, as in
  `resumeflow.lua`: asking again would offer Go over the pages just read.
- Otherwise ask, even at the same spot. Fingerprint: `event:<id>`, else `seq:<n>`, else position.
  Keep the last 40 choices per book. A new event at the same spot asks again.
- Invalid event (wrong book, missing `event_id`, missing `reading_head`) is a reported error, no prompt.
- **Ir al 62%** (**Ir a esa posición** with no percent): save an undo point (single use), resolve the position locally (xpointer, else
  percentage), navigate, enqueue own progress with `explicit_jump:true`.
- **Seguir en el 10%** (**Seguir acá** with no percent): stay, stamp local reading activity now, enqueue own progress: this device becomes
  the head.
- Both remember the choice, release the hold and drain. Pending `suggestion_id` is resolved, in that
  same session or, when the radio was off for it, in the next one that opens that book.
- **`Buscar posición al abrir` off** means this reader says where it is without asking. A hold waits
  for an answer, and with the search off nothing would ever give one: the position would stay
  unsent for good behind a newer head. So there are no holds with it off, and switching it off
  releases the ones there are. A newer head elsewhere gets overwritten by what is read here, which
  is what someone who turned the question off asked for. Manual sync from the drawer still offers.
- A hold with no position queued behind it is given back at the next session that looks at holds, so
  books opened once do not fill the 32 places. Held events are counted on the Borges screen with
  their reason: `Esperan que abras su libro: N`.
- Warnings: backwards (Δ < -0.5%), close (|Δ| ≤ 2%), approximate (no xpointer usable).
- Dates shown only for `exact`/`anchored` events. The remote page number is never shown: convert
  through percentage.
- Replayed `progress.changed` in pulls never prompts. It only updates the per-book remote head cache.
- `Volver a donde estaba` in the reader drawer while an undo point exists.

The prompt opens with the focus on the stay action when going there would move backwards, so the
easy press keeps what was read. Back says `Después`: it answers nothing and asks again next time.
`Sin dato` stands for a place this reader cannot state, because it never opened the book or because
its metadata could not be built, and `En otro lector` for a device with no name.
After a jump the screen says where the way back is: `Volver a donde estaba` in the book menu.

Copy (es): `¿Dónde querés seguir leyendo?` · `En este lector: …` · `En <lector>: …` · `Ir al 62%` /
`Seguir en el 10%` · `Ir ahí te lleva hacia atrás en el libro.` · `La diferencia entre las dos
posiciones es chica.` · `La ubicación es aproximada: puede caer unos renglones antes o después.`

## 6. Wire facts

- Platform `crossink-x4`. `external_id` from the chip MAC, prefixed (`x4-…`), hashed, stable.
  `client_version` `crossink-borges/<ver>`. Pair with a stable `external_id`.
- Poll `pairing/status` every `interval` (5 s) and claim only once `approved`.
- Book identity: `koreader_partial_md5` always. `aggregate_id` for progress is
  `koreader_partial_md5:<hash>`, `aggregate_type` `reading_progress`, `document_id` null.
- Progress payload: `percentage` (0-100, required), `current_page`, `total_pages`, `xpointer`,
  `explicit_jump`. Server-side `current_page ≤ total_pages`.
- Annotation: `aggregate_type:"annotation"`, `aggregate_id` = `payload.sync_id` (UUID), `text`
  required, at least one of `xpointer`/`position`/`page`. Optional `chapter`, `note`, `color`
  (default **gray**), `drawer` (`lighten`, `bookmark`), `time` (epoch ms), `percentage`,
  `book:{title,author,file,total_pages}` (required so the server can create the document). A highlight
  crosses into KOReader only with `xpointer` and `position:{pos0,pos1}`, both resolving, and text
  matching that range (`annotationadapter.lua`, `buildItem`); a bookmark needs `xpointer`. `page` is
  the book-wide reference page, as in progress. `base_revision` on a delete is the last revision seen
  from another device.
- `session.ended`: `started_at`, `ended_at`, `duration_seconds` (60-14400, otherwise do not emit),
  `pages_read`, `start_page`, `end_page`. `aggregate_id` `session:<hash>:<started_at>`.
- `page_stat.recorded`: `aggregate_type:"reading_statistics"`, `aggregate_id`
  `page-stats:<hash>:<first start>:<last start>:<chunk>`, `rows:[{book_md5,page,start_time,duration,
  total_pages}]` and `books:[{md5,title,authors,pages}]`, at most 32 rows per event. The rows live in
  RAM while reading (`PageStatBuffer`) and go out at finalize: this table is what the dashboard's hours,
  heatmap and streaks read, and its `books` entry creates the document for a book with no highlights.
- All 64-bit integers are decimal strings. Ack `accepted` and `rejected` both leave the outbox.
- Library: `GET /api/library/plugin/books?page=&limit=`; `GET …/:id/download` with Range, `ETag`,
  `X-Content-SHA256`, `X-KOReader-Partial-MD5`. Verify partial MD5 before accepting a file.
- Health: `POST /api/devices/self/health` with `outcome`, `queue_depth`, `error_code`.
- Rotate after `renew_after` when known, and keep the one a rotation answers with. Pairing credentials
  may have no expiry; with no `renew_after`, the last 14 days before `expires_at` are the rule. A
  claim or a rotation is never repeated on a dropped socket: the server does each once and cannot
  answer again. `credential_already_rotated` means the new token was lost on its way here: this one
  still works for its grace, nothing recovers it after that, and so the screen asks to pair again now.
- An imported or XOR-era credential sat readable on the card, so it is rotated at the first session
  that can vouch for its clock. Going back to the old firmware after that needs pairing again.
- 401/`device_revoked`: stop automatic sessions, keep the queue, and show "Volvé a vincular este lector".
- Disconnect: no route lets a device revoke itself, so online it rotates with
  `expires_in_seconds:60` and `grace_seconds:0` and discards the result. It sends what is queued first,
  while it still can. Offline, the screen says the server still has the credential. Another account
  on re-pair resets queue, cursor and ledgers, which the confirmation says; the same account keeps
  them (by `account.id`, or by `account.username` when no id is sent), and so does a credential with
  no account to compare, since nothing proves it changed.

## 7. Highlights

CrossInk clippings stay as they are on disk. At finalize time the book's clippings and bookmarks are
compared against the ledger, keyed by a stable hash of (partial MD5, spine, text offset, text hash).
New ones become `annotation.created`, removed ones `annotation.deleted`. `sync_id` is derived once and
stored. The xpointer is resolved with `ChapterXPathResolver` while the book is still loaded. Clip
`timestamp` is uptime, so the wall time is stamped by a hook at save time, or `time` is omitted.
The clippings on the card do not belong to an account, and the ledger does. So the first time an
account opens a book after taking over the card, everything on disk is recorded as seen and nothing
is sent: not the previous person's highlights to this account, and not a highlight deleted on the
web back to its owner as a conflict. Only what is made from then on goes up. A record seeded that way
has nothing on the server to delete, so deleting it locally says nothing. The cost, accepted: someone
who returns to the account they had and deletes an old highlight here deletes it here only. The first
account a card ever has takes over nobody, and publishes what was there.

The same goes for the place: back on an account with the same device id, a head from this device
with no reading since the account returned is offered like anyone else's, because the page on the
card is whoever had it last.

### Highlights from other readers

KOReader applies what another reader or the web highlighted to the book. The firmware cannot draw
one in the page (section 10), so it keeps them per book and lists them: reader drawer
**Subrayados de otros lectores** / **Highlights from other readers**, read-only, and only in a book
that has one. Each row: the text (up to three lines across a portrait screen, two across a landscape one,
ending in an ellipsis when there is more), then chapter, source device and how long ago, each when
known. The date is the highlight's own `time`, else `occurred_at` when its precision is `exact` or
`anchored`, and only with a clock that can tell. Rows go in book order, one fixed height so the focus moves
by a fixed step; a focus move is a fast refresh and a new page a half one. Confirm jumps there.

**What is kept.** The `annotation.*` events with directive `apply_remote`, as the plugin applies:

| Comes in | What happens |
|---|---|
| Newer revision of a highlight | Replaces it. An older revision arriving later says nothing. With no revision on the wire (`metadata_omitted`) arrival order decides |
| `annotation.deleted` | Off the list, and out of the log (it is compacted) before the cursor moves. On FAT that frees the old clusters without overwriting them |
| `annotation_conflict`, any other directive | Not listed: a conflict waits on the web for a person |
| `bookmark.*`, `kind:"bookmark"` | Not listed: the list says highlights |
| No usable `sync_id`, or a text of nothing | Not listed. The cursor moves past all of these |

Notes are not kept or shown: a row has no room for them, and the web has them.

**Own highlights never echo.** An event whose origin is this device is ignored, as ever. A `sync_id`
in this book's clipping ledger is this device's too, whoever touched it last: a web edit of a
highlight made here moves its revision and lists nothing.

**The same passage shows once.** KOReader and this firmware derive different `sync_id`s for the same
selection, so an id cannot tell. A passage is its text compared the way `annotationadapter.lua:comparableText`
does (white space collapsed, NBSP as space, soft hyphens gone), over the first 200 bytes left, plus the
chapter the xpointer names. If this reader has that passage in a clipping, the list leaves it out. If
two other devices do, it shows once, as the newer. The same words elsewhere in the book are another
passage. So a slightly different selection of the same sentence shows as its own row, which is what KOReader
does as well. The drawer's check reads only the clippings in the chapters asked about and is kept in
RAM until the book or its clipping count changes (eight bytes), since a menu opens often.

**Bounds, always true on the card.** Per book, 256 records and 64 KiB; on reaching either, the log
compacts, dropping the oldest, down to 192 and 48 KiB, and the list then says it holds the most recent.
A long text is cut at 480 bytes on a character. Across the card, 200 books and 4 MiB; past that the book used
longest ago goes whole. Measured against the real server: 300 highlights of one book, about 110 bytes of
text each, end as a 56 KB log.

**Crash-safe.** `inbound/<xx>/<rest of md5>` is an append-only log of CRC-framed records flushed one by
one, with the server sequence in each: a redelivered event at or under the newest one is a no-op, and a
tear only costs the record the cursor had not passed. A compaction writes `<name>.n` beside it and ends in
a seal record; a sealed one found at any open wins, an unsealed one is what a crash left and goes. No
rename over a file, which FAT cannot do atomically. `inbound/index.{a,b}` is an `AtomicFile` listing the books, oldest
use first: a book is named there before its log exists, so a crash leaves a name with no file, never a
file nobody can find to count or delete.

**Whose they are.** Each log and the index carry `accountEpoch`. Another epoch reads as empty and is
overwritten at the next write. Another account taking the card deletes them through the index then and there:
these are the first words of a person's own the firmware keeps from the server. Disconnecting keeps them,
as it keeps the queue. They live under `/.crosspoint/borges`, which the transfer servers refuse.

**Memory.** Nothing resident while reading. A pull holds one record and its encoding for the session
(about 2.8 KB, allocated at the first page, so never during a finalize with a book loaded), and 4 KB for the moment
a compaction or an index update takes. Pull parsing is unchanged: one element at a time in the fixed window, the
parse budget still skipping the rest. The list holds 16 B per highlight and one record (about 1.2 KB) while it
is open, plus up to 4 KB while it opens, and nothing after.

**The jump.** Before leaving, where the reader is becomes the undo point, unless a jump was made and nothing
read since, in which case the way back stays where the reading was. With the framebuffer lent, as for an
exit, the mapper resolves the xpointer. When it cannot, the percentage is used (from `percentage`, else from
`page`/`total_pages`), never outside the chapter the xpointer names; the screen says the place is approximate. With
neither, or with no place in this book, it says `No pude ir a ese subrayado` over the same page for a moment and
nothing moved. A jump counts as any other way to a page, as in the plugin: the place is published as this
reader's (not as `explicit_jump`, since nobody asked for it to win over another device), and `Volver a donde
estaba` publishes the way back. Everything else about the reader is untouched: it reopens the book at that
place, as after `Volver a donde estaba`.

## 8. Testing

1. Host unit tests of `lib/BorgesCore` (GoogleTest, CMake) with an in-memory FS that injects crashes at
   every write and torn writes, plus a scripted HTTP fake.
2. Contract tests: the same core, compiled as a CLI, against the real Borges server on a throwaway
   PostgreSQL 17, with a second client behaving as KOReader.
3. Simulator end to end: the real firmware on Windows, scripted buttons, screenshots.
4. TLS probe: wolfSSL built with the device's exact configuration and anchors, against production.
5. Firmware build with the size gate. Not covered without hardware: real radio, real heap, panel.

## 9. Accepted risks

**OTA has no firmware signature.** An update is checked against the SHA-256 and the size in the
release manifest, its chip and board tag, and it comes from GitHub's API over verified TLS. That
makes the trust root whoever can publish a release on `runadev-arg/borges-crossink`. Anyone holding
that repository can ship firmware to every reader. A signature would move the trust to an offline key
and is not implemented.

**The file transfer server has no authentication.** It is a mode a person starts, on their own
network. `/.crosspoint/borges` and `/.borges` are refused there, over WebDAV and over USB, even with
Show Hidden Files on (`logic/PrivatePaths`); the rest of the card is as open as it always was. A rename
of the parent folder `/.crosspoint` is still possible with hidden files shown, and it would put those
files at another path until the next boot. What is there is sealed with a key on the chip, except
for an old credential in `/.borges`, which is refused directly and retired by rotation.

## 10. Scope limits

Boots only as the X3/X4 `default` image. Other boards still compile because `src/borges` builds on
SDK abstractions, but they are not validated.

Rendering highlights inside the page is out of scope, both this device's and other readers'. Other
readers' are reachable from the reader drawer (section 7), and that list is read-only: nothing can be
edited, deleted or adopted from it, and it has no touch handling beyond what the drawer has.
