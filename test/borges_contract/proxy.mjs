// Reverse proxy between the clients and the real server. Everything goes through it, so
// every request is logged and any one of them can be made to fail at a chosen point.
//
// It also sets X-Forwarded-For (the server runs with TRUST_PROXY_HOPS=1): the server's
// rate limits are per IP and in memory, and a test needs to choose when it hits them.

import http from "node:http";

const HOP_BY_HOP = new Set(["connection", "keep-alive", "transfer-encoding", "te", "upgrade", "proxy-connection", "trailer"]);

export class FaultProxy {
  constructor({ upstreamPort, port = 0 }) {
    this.upstreamPort = upstreamPort;
    this.port = port;
    this.rules = [];
    this.log = [];
    this.clientIp = "10.0.0.1";
    this.nextIp = 2;
    this.agent = new http.Agent({ keepAlive: true, maxSockets: 8 });
    this.server = null;
    this.sockets = new Set();
    this.connections = 0;
    this.requests = 0;
  }

  get url() {
    return `http://127.0.0.1:${this.port}`;
  }

  freshIp() {
    const n = this.nextIp++;
    this.clientIp = `10.${(n >> 16) & 255}.${(n >> 8) & 255}.${n & 255}`;
    return this.clientIp;
  }

  async up() {
    if (this.server) return;
    this.server = http.createServer((req, res) => this.handle(req, res));
    this.server.keepAliveTimeout = 0;
    this.server.headersTimeout = 0;
    this.server.requestTimeout = 0;
    this.server.on("connection", (socket) => {
      this.connections += 1;
      this.sockets.add(socket);
      socket.on("close", () => this.sockets.delete(socket));
    });
    await new Promise((resolve, reject) => {
      this.server.once("error", reject);
      this.server.listen(this.port, "127.0.0.1", resolve);
    });
    this.port = this.server.address().port;
  }

  // Connections are refused from here until up().
  async down() {
    if (!this.server) return;
    const server = this.server;
    this.server = null;
    for (const socket of this.sockets) socket.destroy();
    await new Promise((resolve) => server.close(resolve));
  }

  clear() {
    this.rules = [];
    this.log = [];
  }

  // A rule fires on the nth matching request (default the first) and, with times, keeps
  // firing for that many. match: string or RegExp on the path. method is optional.
  fault(rule) {
    const stored = { nth: 1, times: 1, seen: 0, fired: 0, ...rule };
    this.rules.push(stored);
    return stored;
  }

  // Fires on the nth request of whatever kind, counted from now.
  faultAtRequest(nth, rule) {
    return this.fault({ ...rule, match: /./, nth });
  }

  takeRule(method, path) {
    for (const rule of this.rules) {
      if (rule.fired >= rule.times) continue;
      if (rule.method && rule.method !== method) continue;
      const hit = rule.match instanceof RegExp ? rule.match.test(path) : path === rule.match || path.startsWith(`${rule.match}?`);
      if (!hit) continue;
      rule.seen += 1;
      if (rule.seen < rule.nth) continue;
      rule.fired += 1;
      return rule;
    }
    return null;
  }

  requestsTo(pathOrRe) {
    return this.log.filter((e) => (pathOrRe instanceof RegExp ? pathOrRe.test(e.path) : e.path === pathOrRe));
  }

  handle(req, res) {
    const chunks = [];
    req.on("data", (c) => chunks.push(c));
    req.on("error", () => {});
    res.on("error", () => {});
    req.on("end", () => {
      const body = Buffer.concat(chunks);
      this.process(req, res, body).catch((error) => {
        if (!res.headersSent) {
          res.writeHead(502, { "content-type": "application/json" });
          res.end(JSON.stringify({ error: { code: "contract_proxy_error", message: String(error.message) } }));
        } else {
          req.socket.destroy();
        }
      });
    });
  }

  async process(req, res, body) {
    this.requests += 1;
    const entry = {
      method: req.method,
      path: req.url,
      headers: req.headers,
      body: body.toString("utf8"),
      bodyBytes: body.length,
      status: 0,
      responseBody: null,
      fault: null,
    };
    this.log.push(entry);
    const rule = this.takeRule(req.method, req.url);
    if (rule) entry.fault = rule.action;

    if (rule?.delayMs) await new Promise((r) => setTimeout(r, rule.delayMs));

    if (rule?.action === "drop-before") {
      req.socket.destroy();
      return;
    }
    if (rule?.action === "status" && !rule.forward) {
      this.reply(res, entry, rule);
      return;
    }

    const upstream = await this.forward(req, body);
    entry.status = upstream.status;
    entry.responseBody = upstream.body.length <= 256 * 1024 ? upstream.body.toString("utf8") : null;
    entry.responseHeaders = upstream.headers;

    switch (rule?.action) {
      case "drop-after":
        // The server committed. The client never hears about it.
        req.socket.destroy();
        return;
      case "status":
        this.reply(res, entry, rule);
        return;
      case "truncate": {
        const keep = rule.bytes ?? Math.floor(upstream.body.length * (rule.fraction ?? 0.5));
        res.writeHead(upstream.status, upstream.headers);
        res.write(upstream.body.subarray(0, keep), () => setTimeout(() => req.socket.destroy(), 5));
        return;
      }
      case "truncate-headers": {
        const head = `HTTP/1.1 ${upstream.status} OK\r\ncontent-type: appli`;
        req.socket.write(head, () => req.socket.destroy());
        return;
      }
      case "corrupt": {
        const copy = Buffer.from(upstream.body);
        for (const offset of rule.offsets ?? [0]) {
          if (offset < copy.length) copy[offset] ^= 0x5a;
        }
        res.writeHead(upstream.status, upstream.headers);
        res.end(copy);
        return;
      }
      case "rewrite": {
        const rewritten = Buffer.from(JSON.stringify(rule.rewrite(JSON.parse(upstream.body.toString("utf8")))));
        res.writeHead(upstream.status, { ...upstream.headers, "content-length": String(rewritten.length) });
        res.end(rewritten);
        return;
      }
      case "garbage":
        res.writeHead(200, { "content-type": "application/json", "content-length": "11" });
        res.end("<html>oops!");
        return;
      default:
        res.writeHead(upstream.status, upstream.headers);
        res.end(upstream.body);
    }
  }

  reply(res, entry, rule) {
    const payload = rule.body ?? JSON.stringify({ error: { code: rule.code ?? "contract_injected", message: "injected", retryable: true } });
    entry.status = rule.status;
    entry.responseBody = payload;
    res.writeHead(rule.status, {
      "content-type": "application/json",
      "content-length": Buffer.byteLength(payload),
      ...(rule.retryAfter != null ? { "retry-after": String(rule.retryAfter) } : {}),
    });
    res.end(payload);
  }

  forward(req, body) {
    return new Promise((resolve, reject) => {
      const headers = {};
      for (const [name, value] of Object.entries(req.headers)) {
        if (!HOP_BY_HOP.has(name)) headers[name] = value;
      }
      headers["x-forwarded-for"] = this.clientIp;
      headers.host = `127.0.0.1:${this.port}`;
      if (body.length) headers["content-length"] = String(body.length);
      const out = http.request(
        { host: "127.0.0.1", port: this.upstreamPort, method: req.method, path: req.url, headers, agent: this.agent },
        (upstream) => {
          const chunks = [];
          upstream.on("data", (c) => chunks.push(c));
          upstream.on("error", reject);
          upstream.on("end", () => {
            const payload = Buffer.concat(chunks);
            const responseHeaders = {};
            for (const [name, value] of Object.entries(upstream.headers)) {
              if (!HOP_BY_HOP.has(name)) responseHeaders[name] = value;
            }
            responseHeaders["content-length"] = String(payload.length);
            resolve({ status: upstream.statusCode, headers: responseHeaders, body: payload });
          });
        }
      );
      out.on("error", reject);
      if (body.length) out.write(body);
      out.end();
    });
  }
}
