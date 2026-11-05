#!/usr/bin/env python3
"""A scripted stand-in for the Borges hub, for simulator screenshots only.

Not a protocol test: test/borges_contract does that against the real server. This
only says enough for each Borges screen to reach the state being photographed.

    python scripts/sim/borges_fake_server.py --port 18090 --book test.epub --scenario prompt

Scenarios:
  prompt      The focus event is ahead of this reader, on another device.
  backwards   The focus event is behind this reader, with no usable xpointer.
  quiet       Nobody else read this book.
  own         The focus event is this device's own (--device-id): a card moved to new firmware.
  fail        Every sync request answers 500, with a request id.
"""
import argparse
import hashlib
import json
import os
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def partial_md5(path):
    """KOReader's partial MD5: 1 KiB at 0 and at 1024 << 2i for i in 0..10."""
    digest = hashlib.md5()
    with open(path, "rb") as f:
        size = os.path.getsize(path)
        for i in range(-1, 11):
            offset = 0 if i < 0 else 1024 << (2 * i)
            if offset >= size:
                break
            f.seek(offset)
            digest.update(f.read(1024))
    return digest.hexdigest()


ARGS = None
BOOK_MD5 = ""
HOSTILE_TITLE = "Una historia universal de la infamia, con un título larguísimo que no entra ni en dos renglones"


def library_page(page):
    books = [
        {
            "id": "b1",
            "koreader_partial_md5": BOOK_MD5,
            "title": "Ficciones",
            "author": "Jorge Luis Borges",
            "original_filename": os.path.basename(ARGS.book),
            "file_size": os.path.getsize(ARGS.book),
            "status": "ready",
        },
        {
            "id": "b2",
            "koreader_partial_md5": "0" * 31 + "2",
            "title": HOSTILE_TITLE,
            "author": "Autor con un nombre también bastante más largo de lo razonable",
            "original_filename": "../../etc/passwd",
            "file_size": 1834567,
            "status": "ready",
        },
    ]
    for i in range(3, 30):
        books.append(
            {
                "id": f"b{i}",
                "koreader_partial_md5": f"{i:032x}",
                "title": f"Libro número {i}: ñandú y acentuación",
                "author": "Sara Gallardo",
                "original_filename": f"libro-{i}.epub",
                "file_size": 1000 + i * 31337,
                "status": "ready",
            }
        )
    return books, page


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    status_calls = 0

    def log_message(self, fmt, *args):
        print("fake:", self.command, self.path, flush=True)

    def reply(self, status, body):
        data = json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("X-Request-Id", "req-7f3a9c")
        self.end_headers()
        self.wfile.write(data)

    def pull_body(self):
        body = {
            "protocol_version": 2,
            "cursor": "10",
            "has_more": False,
            "events": [],
            "reading_head_checked": True,
            "server_time": "2026-11-05T15:00:00Z",
            "focus_event": None,
        }
        if ARGS.scenario in ("prompt", "backwards", "own"):
            ahead = ARGS.scenario != "backwards"
            body["focus_event"] = {
                "event_id": f"progress:{ARGS.scenario}:{int(time.time())}",
                "server_sequence": "9",
                "event_type": "progress.changed",
                "book_identifier": {"kind": "koreader_partial_md5", "value": BOOK_MD5},
                "origin_device": {"id": ARGS.device_id, "name": "X4"} if ARGS.scenario == "own"
                else {"id": "other-device", "name": "Kobo Libra de Toto"},
                "occurred_at": "2026-11-05T13:00:00Z",
                "time_precision": "exact",
                "payload": {
                    "percentage": 62.5 if ahead else 0.5,
                    "current_page": 200,
                    "total_pages": 320,
                },
                "directive": "reading_head",
                "directive_metadata": {"reading_head": True},
            }
            if ahead:
                body["focus_event"]["payload"]["xpointer"] = "/body/DocFragment[2]/body/p[3]/text().0"
        return body

    def read_body(self):
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length else b""

    def do_POST(self):
        raw = self.read_body()
        path = self.path.split("?")[0]
        if path == "/api/devices/pairing/request":
            self.reply(200, {"request_id": "11111111-2222-4333-8444-555555555555", "user_code": "K7MP-2XQ9",
                             "verification_url": ARGS.base + "/devices/pair",
                             "expires_in": 600, "interval": 2})
        elif path == "/api/devices/pairing/claim":
            self.reply(200, {"credential": {"token": "fake-token", "username": "toto"},
                             "device": {"id": "dev1"}, "account": {"id": "acc12345-aaaa"}})
        elif path == "/api/devices/pairing/status":
            Handler.status_calls += 1
            done = Handler.status_calls >= ARGS.approve_after
            self.reply(200, {"status": "approved" if done else "pending"})
        elif ARGS.scenario == "fail":
            self.reply(500, {"error": {"code": "internal_error", "message": "boom"}})
        elif path == "/api/sync/v2/pull":
            time.sleep(ARGS.delay)
            self.reply(200, self.pull_body())
        elif path == "/api/sync/v2/exchange":
            events = json.loads(raw or b"{}").get("events", [])
            for e in events:
                print("fake: event", json.dumps(e, ensure_ascii=False), flush=True)
            acks = [{"client_event_id": e.get("client_event_id"), "client_sequence": e.get("client_sequence"),
                     "status": "accepted"} for e in events]
            self.reply(200, {"acknowledgements": acks, "pull": {**self.pull_body(), "focus_event": None}})
        else:
            self.reply(200, {"ok": True})

    def do_GET(self):
        path, _, query = self.path.partition("?")
        if path == "/api/sync/v2/capabilities":
            versions = {k: [1] for k in ("progress.changed", "annotation.created", "annotation.deleted",
                                         "bookmark.upserted", "bookmark.deleted", "session.ended",
                                         "page_stat.recorded")}
            self.reply(200, {"protocol_version": 2, "event_versions": versions})
        elif path == "/api/sync/v2/progress/positions":
            self.reply(200, {"protocol_version": 2, "positions": [
                {"device_id": ARGS.device_id, "device_name": "X4", "percentage": 10, "is_current_device": True,
                 "recovery_id": "progress:self:1", "xpointer": None},
                {"device_id": "other-device", "device_name": "Kobo Libra de Toto", "percentage": 41.5,
                 "xpointer": "/body/DocFragment[3]/body/p[2]/text().0", "occurred_at": "2026-11-05T13:00:00.000Z",
                 "time_precision": "exact", "is_current_device": False, "recovery_id": "progress:other:7"}]})
        elif path == "/api/library/plugin/books":
            params = dict(p.split("=") for p in query.split("&") if "=" in p)
            page = int(params.get("page", 1))
            limit = int(params.get("limit", 20))
            if ARGS.empty_library:
                self.reply(200, {"books": [], "page": 1, "has_next": False, "has_previous": False})
                return
            books, _ = library_page(page)
            start = (page - 1) * limit
            chunk = books[start:start + limit]
            self.reply(200, {"books": chunk, "page": page, "has_next": start + limit < len(books),
                             "has_previous": page > 1})
        elif path.startswith("/api/library/plugin/books/") and path.endswith("/download"):
            with open(ARGS.book, "rb") as f:
                data = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "application/epub+zip")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("ETag", '"fake-etag"')
            self.send_header("X-KOReader-Partial-MD5", BOOK_MD5)
            self.end_headers()
            self.wfile.write(data)
        else:
            self.reply(404, {"error": {"code": "not_found", "message": self.path}})


def main():
    global ARGS, BOOK_MD5
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=18090)
    parser.add_argument("--book", required=True)
    parser.add_argument("--scenario", default="quiet")
    parser.add_argument("--approve-after", type=int, default=10 ** 6)
    parser.add_argument("--empty-library", action="store_true")
    parser.add_argument("--device-id", default="dev1")
    parser.add_argument("--delay", type=float, default=0, help="Seconds a pull takes, to photograph the wait.")
    ARGS = parser.parse_args()
    ARGS.base = f"http://127.0.0.1:{ARGS.port}"
    BOOK_MD5 = partial_md5(ARGS.book)
    print("fake: book md5", BOOK_MD5, flush=True)
    ThreadingHTTPServer(("127.0.0.1", ARGS.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
