"""The Borges endpoints a session touches, on 127.0.0.1, for QEMU's user network (10.0.2.2).

    python -I fake_server.py --workdir <dir> [--page-bytes N] [--pages N]

Plain HTTP on 8080 hands out the test root. HTTPS on 8443 serves a chain with the shape of
production's: P-256 leaf <- P-384 intermediate <- P-384 root cross-signed by another P-384
<- that one cross-signed by RSA, with verification ending at the root the device holds.
Every key is generated into --workdir at start and is trusted by nothing but the test image.

No account, no data: it answers from nothing and remembers acknowledgements in memory.
"""
import argparse
import json
import re
import ssl
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

OPENSSL = r"C:\Program Files\Git\usr\bin\openssl.exe"
MD5 = "0123456789abcdef0123456789abcdef"
NOW = "2026-11-05T12:00:00.000Z"


def run(*args, cwd):
    subprocess.run([OPENSSL, *args], cwd=cwd, check=True, capture_output=True)


def make_certs(work: Path):
    work.mkdir(parents=True, exist_ok=True)
    # Production's first flight is about 5.3 KB (a 919 B leaf plus CT and a cross chain).
    # Names pad the leaf until the record the client must buffer is that size too.
    padding = ",".join(f"DNS:pad{i:03d}.runadev.example" for i in range(115))
    (work / "ca.ext").write_text(
        "[ca]\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n"
        "subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid\n"
    )
    (work / "leaf.ext").write_text(
        "[leaf]\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\n"
        "extendedKeyUsage=serverAuth\nsubjectAltName=DNS:10.0.2.2,IP:10.0.2.2\n"
        "subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid\n"
    )

    def key(name, *spec):
        run("genpkey", "-out", f"{name}.key", *spec, cwd=work)

    p384 = ("-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:secp384r1")
    p256 = ("-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:prime256v1")
    key("x1", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:4096")
    key("x2", *p384)
    key("rootye", *p384)
    key("ye2", *p384)
    key("leaf", *p256)

    def self_signed(name, subject, digest):
        run("req", "-new", "-x509", "-key", f"{name}.key", "-subj", subject, f"-{digest}", "-days", "3650",
            "-out", f"{name}.self.pem", "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign", cwd=work)

    def signed(name, subject, issuer_cert, issuer_key, digest, ext_file, section, out):
        run("req", "-new", "-key", f"{name}.key", "-subj", subject, "-out", f"{name}.csr", cwd=work)
        run("x509", "-req", "-in", f"{name}.csr", "-CA", issuer_cert, "-CAkey", issuer_key, "-CAcreateserial",
            f"-{digest}", "-days", "365", "-extfile", ext_file, "-extensions", section, "-out", out, cwd=work)

    self_signed("x1", "/O=QEMU Test/CN=Test Root X1", "sha256")
    self_signed("x2", "/O=QEMU Test/CN=Test Root X2", "sha384")
    self_signed("rootye", "/O=QEMU Test/CN=Test Root YE", "sha384")
    signed("x2", "/O=QEMU Test/CN=Test Root X2", "x1.self.pem", "x1.key", "sha256", "ca.ext", "ca", "x2.cross.pem")
    signed("rootye", "/O=QEMU Test/CN=Test Root YE", "x2.self.pem", "x2.key", "sha384", "ca.ext", "ca", "rootye.cross.pem")
    signed("ye2", "/O=QEMU Test/CN=Test YE2", "rootye.self.pem", "rootye.key", "sha384", "ca.ext", "ca", "ye2.pem")
    signed("leaf", "/CN=10.0.2.2", "ye2.pem", "ye2.key", "sha384", "leaf.ext", "leaf", "leaf.pem")
    chain = "".join((work / f).read_text() for f in ("leaf.pem", "ye2.pem", "rootye.cross.pem", "x2.cross.pem"))
    (work / "chain.pem").write_text(chain)


class State:
    page_bytes = 0
    pages = 5
    page_events = 20
    lock = threading.Lock()
    acked = 0


def remote_event(seq: int, filler: int) -> str:
    if seq % 4 != 0:  # Mostly another device's progress on other books, as real feeds are.
        event = focus_event()
        event["directive_metadata"] = {}
        event.update(event_id=f"evt-{seq}", server_sequence=str(seq), aggregate_id=f"p{seq}",
                     book_identifier={"kind": "koreader_partial_md5", "value": f"{seq % 4:032x}"})
        event["payload"]["pad"] = "x" * filler
        return json.dumps(event, separators=(",", ":"))
    return json.dumps({
        "event_id": f"evt-{seq}", "server_sequence": str(seq),
        "origin_device": {"id": "other-device", "name": "Kobo", "platform": "kobo"},
        "client_event_id": "30000000-0000-4000-8000-000000000001", "client_sequence": "1", "event_version": 1,
        "event_type": "annotation.created", "aggregate_type": "annotation", "aggregate_id": f"a{seq}",
        "document_id": None, "book_identifier": {"kind": "koreader_partial_md5", "value": MD5},
        "occurred_at": "2026-11-05T10:00:00.000Z", "time_precision": "exact", "monotonic_ms": None,
        "payload": {"sync_id": "50000000-0000-4000-8000-000000000001", "text": "x" * filler, "chapter": "Uno",
                    "note": "nota", "page": 7},
        "received_at": "2026-11-05T10:00:01.000Z", "directive": "apply_remote", "directive_metadata": {},
    }, separators=(",", ":"))


def focus_event() -> dict:
    return {
        "event_id": "evt-focus", "server_sequence": "1",
        "origin_device": {"id": "other-device", "name": "Kobo", "platform": "kobo"},
        "client_event_id": "30000000-0000-4000-8000-000000000002", "client_sequence": "1", "event_version": 1,
        "event_type": "progress.changed", "aggregate_type": "reading_progress", "aggregate_id": "x",
        "document_id": None, "book_identifier": {"kind": "koreader_partial_md5", "value": MD5},
        "occurred_at": "2026-11-05T10:00:00.000Z", "time_precision": "exact", "monotonic_ms": None,
        "payload": {"percentage": 42.0, "current_page": 100, "total_pages": 320,
                    "xpointer": "/body/DocFragment[3]/body/div/p[12]/text().0", "explicit_jump": False},
        "received_at": "2026-11-05T10:00:01.000Z", "directive": "suggest_resume", "directive_metadata": {"reading_head": True},
    }


def page_json(cursor: int, limit: int) -> dict:
    page = (cursor // State.page_events) if State.page_events else 0
    if page >= State.pages:
        return {"events": [], "cursor": str(cursor), "high_watermark": str(cursor), "has_more": False}
    limit = min(limit, State.page_events)
    filler = max(0, (State.page_bytes // limit) - 700) if State.page_bytes else 300
    events = [remote_event(cursor + i + 1, filler) for i in range(limit)]
    next_cursor = cursor + limit
    return {"__events_raw": events, "cursor": str(next_cursor),
            "high_watermark": str(State.pages * State.page_events), "has_more": page + 1 < State.pages}


def dump(obj: dict) -> bytes:
    raw = obj.pop("__events_raw", None)
    text = json.dumps(obj, separators=(",", ":"))
    if raw is not None:
        text = text[:-1] + ',"events":[' + ",".join(raw) + "]}"
    return text.encode()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "fake"

    def log_message(self, fmt, *args):
        print(self.requestline, "->", args[1] if len(args) > 1 else "", flush=True)

    def reply(self, status: int, body: bytes, ctype="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Request-Id", "fake-0001")
        self.end_headers()
        self.wfile.write(body)

    def do_HEAD(self):
        self.reply(200, b"")

    def do_GET(self):
        if self.path == "/root.pem":
            return self.reply(200, (self.server.workdir / "rootye.self.pem").read_bytes(), "text/plain")
        if self.path == "/x1.pem":
            return self.reply(200, (self.server.workdir / "x1.self.pem").read_bytes(), "text/plain")
        match = re.match(r"/big\?n=(\d+)", self.path)
        if match:
            return self.reply(200, b"z" * int(match.group(1)), "application/octet-stream")
        if self.path == "/api/sync/v2/capabilities":
            return self.reply(200, json.dumps({
                "protocol_version": 2, "supported_protocol_versions": [2],
                "event_versions": {k: [1] for k in ("progress.changed", "session.started", "session.ended",
                                                    "page_stat.recorded", "annotation.created", "annotation.updated",
                                                    "annotation.deleted", "bookmark.upserted", "bookmark.deleted",
                                                    "device.library_reported")},
                "limits": {"push_events": 100}, "cursor": {"encoding": "decimal_string"}}).encode())
        self.reply(404, b'{"error":{"code":"not_found"}}')

    def do_POST(self):
        size = int(self.headers.get("Content-Length") or 0)
        body = json.loads(self.rfile.read(size) or b"{}")
        if self.path == "/api/sync/v2/pull":
            cursor = int(body.get("cursor") or 0)
            if body.get("focus_only"):
                return self.reply(200, dump({
                    "protocol_version": 2, "focus_only": True, "reading_head_checked": True,
                    "focus_event": focus_event(), "events": [], "cursor": str(cursor), "has_more": False,
                    "server_time": NOW}))
            page = page_json(cursor, int(body.get("limit") or 20))
            return self.reply(200, dump({"protocol_version": 2, "server_time": NOW, **page}))
        if self.path == "/api/sync/v2/exchange":
            cursor = int(body.get("cursor") or 0)
            acks = []
            with State.lock:
                for event in body.get("events", []):
                    State.acked += 1
                    acks.append({"client_event_id": event["client_event_id"],
                                 "client_sequence": event["client_sequence"], "server_sequence": str(1000 + State.acked),
                                 "status": "accepted", "rejection_code": None, "rejection_detail": None,
                                 "metadata": {}, "duplicate": False})
            page = page_json(cursor, int(body.get("pull_limit") or 20))
            raw = page.pop("__events_raw", None)
            pull = json.dumps(page, separators=(",", ":"))
            if raw is not None:
                pull = pull[:-1] + ',"events":[' + ",".join(raw) + "]}"
            text = '{"protocol_version":2,"acknowledgements":' + json.dumps(acks, separators=(",", ":")) + \
                   ',"pull":' + pull + ',"server_time":"' + NOW + '"}'
            return self.reply(200, text.encode())
        if self.path == "/api/sync/v2/cursor/commit":
            return self.reply(200, json.dumps({"protocol_version": 2, "cursor": body.get("cursor", "0")}).encode())
        if self.path == "/api/devices/self/health":
            return self.reply(200, b'{"device":{}}')
        if self.path.startswith("/api/sync/v2/suggestions/"):
            return self.reply(200, b'{"id":"x"}')
        self.reply(404, b'{"error":{"code":"not_found"}}')


class TlsServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, handler, workdir, context=None):
        super().__init__(address, handler)
        self.workdir = workdir
        self.context = context

    def get_request(self):
        sock, addr = super().get_request()
        if self.context is not None:
            sock = self.context.wrap_socket(sock, server_side=True)
        return sock, addr

    def handle_error(self, request, client_address):
        pass  # A client running out of memory on purpose hangs up in the middle.


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--workdir", required=True)
    parser.add_argument("--page-bytes", type=int, default=0)
    parser.add_argument("--pages", type=int, default=5)
    args = parser.parse_args()
    work = Path(args.workdir)
    if not (work / "chain.pem").exists():
        make_certs(work)
    State.page_bytes = args.page_bytes
    State.pages = args.pages

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.load_cert_chain(work / "chain.pem", work / "leaf.key")
    plain = TlsServer(("127.0.0.1", 8080), Handler, work)
    secure = TlsServer(("127.0.0.1", 8443), Handler, work, context)
    threading.Thread(target=plain.serve_forever, daemon=True).start()
    print("fake server: http 8080, https 8443, ready", flush=True)
    secure.serve_forever()


if __name__ == "__main__":
    main()
