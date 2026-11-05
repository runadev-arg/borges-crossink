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

Never: Wi-Fi on while reading, an error dialog interrupting reading, a jump without consent.

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
- **Ir a esa posición**: save an undo point (single use), resolve the position locally (xpointer, else
  percentage), navigate, enqueue own progress with `explicit_jump:true`.
- **Seguir aquí**: stay, stamp local reading activity now, enqueue own progress: this device becomes
  the head.
- Both remember the choice, release the hold and drain. Pending `suggestion_id` is resolved.
- Warnings: backwards (Δ < -0.5%), close (|Δ| ≤ 2%), approximate (no xpointer usable).
- Dates shown only for `exact`/`anchored` events. The remote page number is never shown: convert
  through percentage.
- Replayed `progress.changed` in pulls never prompts. It only updates the per-book remote head cache.
- `Volver a donde estaba` in the reader drawer while an undo point exists.

Copy (es): `¿Dónde querés seguir leyendo?` · `En este lector: …` · `En <lector>: …` · `Ir a esa
posición` · `Seguir aquí` · `Ir ahí te lleva hacia atrás en el libro.` · `La diferencia entre las dos
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
  `book:{title,author,file,total_pages}` (required so the server can create the document).
- `session.ended`: `started_at`, `ended_at`, `duration_seconds` (60-14400, otherwise do not emit),
  `pages_read`, `start_page`, `end_page`. `aggregate_id` `session:<hash>:<started_at>`.
- All 64-bit integers are decimal strings. Ack `accepted` and `rejected` both leave the outbox.
- Library: `GET /api/library/plugin/books?page=&limit=`; `GET …/:id/download` with Range, `ETag`,
  `X-Content-SHA256`, `X-KOReader-Partial-MD5`. Verify partial MD5 before accepting a file.
- Health: `POST /api/devices/self/health` with `outcome`, `queue_depth`, `error_code`.
- Rotate after `renew_after` when known. Pairing credentials may have no expiry.
- 401/`device_revoked`: stop automatic sessions, keep the queue, and show "Volvé a conectar".
- Disconnect: no route lets a device revoke itself, so online it rotates with
  `expires_in_seconds:60` and `grace_seconds:0` and discards the result. Offline, the screen says the
  server still has the credential. Another account on re-pair resets queue, cursor and ledgers; the
  same account keeps them (by `account.id`, or by `account.username` when no id is sent).

## 7. Highlights

CrossInk clippings stay as they are on disk. At finalize time the book's clippings and bookmarks are
compared against the ledger, keyed by a stable hash of (partial MD5, spine, text offset, text hash).
New ones become `annotation.created`, removed ones `annotation.deleted`. `sync_id` is derived once and
stored. The xpointer is resolved with `ChapterXPathResolver` while the book is still loaded. Clip
`timestamp` is uptime, so the wall time is stamped by a hook at save time, or `time` is omitted.
Inbound annotations from other devices advance the cursor and are not stored: nothing on the device
shows them, and keeping them would cost up to 32 MB at 500 books for a list nobody can open.

## 8. Testing

1. Host unit tests of `lib/BorgesCore` (GoogleTest, CMake) with an in-memory FS that injects crashes at
   every write and torn writes, plus a scripted HTTP fake.
2. Contract tests: the same core, compiled as a CLI, against the real Borges server on a throwaway
   PostgreSQL 17, with a second client behaving as KOReader.
3. Simulator end to end: the real firmware on Windows, scripted buttons, screenshots.
4. TLS probe: wolfSSL built with the device's exact configuration and anchors, against production.
5. Firmware build with the size gate. Not covered without hardware: real radio, real heap, panel.

## 9. Scope limits

Boots only as the X3/X4 `default` image. Other boards still compile because `src/borges` builds on
SDK abstractions, but they are not validated. Rendering highlights inside the page is out of scope.
