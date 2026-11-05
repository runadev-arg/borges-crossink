// The contract harness's server, on directories of its own. Its production guard is reused
// untouched: guard.mjs is preloaded into the migration and the server, safety.mjs decides
// about the database, and proveSafetyHolds() runs before anything of the server does.
//
// Nothing of test/borges_contract's fixed directories is touched, since a contract run may
// be going on at the same moment.

import { execFileSync, spawn } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import net from "node:net";
import path from "node:path";
import { createRequire } from "node:module";
import { fileURLToPath, pathToFileURL } from "node:url";
import { FaultProxy } from "../borges_contract/proxy.mjs";
import { Harness, KOBO } from "../borges_contract/harness.mjs";
import { KoreaderSim } from "../borges_contract/koreader.mjs";
import { assertLocalTestDatabaseUrl, selfTestSafety } from "../borges_contract/safety.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const GUARD = pathToFileURL(path.join(HERE, "..", "borges_contract", "guard.mjs")).href;
const PG_BIN = "C:\\Program Files\\PostgreSQL\\17\\bin";
const DATABASE = "borges_e2e_test";

export const E2E_ROOT = "D:\\CrossInkBorges\\tmp\\e2e";
export const CLUSTER = path.join(E2E_ROOT, "pg");
export const RUN_DIR = path.join(E2E_ROOT, "run");

function freePort() {
  return new Promise((resolve, reject) => {
    const s = net.createServer();
    s.once("error", reject);
    s.listen(0, "127.0.0.1", () => {
      const { port } = s.address();
      s.close(() => resolve(port));
    });
  });
}

const pgEnv = () => ({ SystemRoot: process.env.SystemRoot, PATH: `${PG_BIN};${process.env.SystemRoot}\\System32`, TEMP: E2E_ROOT, TMP: E2E_ROOT, PGCLIENTENCODING: "UTF8" });
const pgctl = (args) => execFileSync(path.join(PG_BIN, "pg_ctl.exe"), args, { env: pgEnv(), stdio: "ignore", windowsHide: true });

// Adds what the device download tests need and the contract proxy never had to do:
// answering slowly, and dropping the connection part way through a body.
export class E2eProxy extends FaultProxy {
  async process(req, res, body) {
    const rule = this.peekStreamRule(req.method, req.url);
    if (!rule) return super.process(req, res, body);
    rule.fired += 1;
    this.requests += 1;
    const entry = { method: req.method, path: req.url, headers: req.headers, body: "", bodyBytes: body.length, status: 0, responseBody: null, fault: rule.action };
    this.log.push(entry);
    const upstream = await this.forward(req, body);
    entry.status = upstream.status;
    entry.responseHeaders = upstream.headers;
    res.writeHead(upstream.status, upstream.headers);
    const limit = rule.action === "cut" ? (rule.bytes ?? Math.floor(upstream.body.length * (rule.fraction ?? 0.5))) : upstream.body.length;
    const perTick = Math.max(1, Math.floor((rule.bytesPerSec ?? 1e9) / 20));
    let sent = 0;
    entry.sent = 0;
    while (sent < limit && !res.destroyed && !req.socket.destroyed) {
      const piece = upstream.body.subarray(sent, Math.min(limit, sent + perTick));
      await new Promise((resolve) => res.write(piece, resolve));
      sent += piece.length;
      entry.sent = sent;
      if (rule.bytesPerSec) await new Promise((r) => setTimeout(r, 50));
    }
    if (rule.action === "cut") {
      await new Promise((r) => setTimeout(r, 5));
      req.socket.destroy();
    } else {
      res.end();
    }
  }

  peekStreamRule(method, url) {
    for (const rule of this.rules) {
      if (rule.action !== "throttle" && rule.action !== "cut") continue;
      if (rule.fired >= rule.times) continue;
      if (rule.method && rule.method !== method) continue;
      const hit = rule.match instanceof RegExp ? rule.match.test(url) : url === rule.match;
      if (hit) return rule;
    }
    return null;
  }
}

export class E2eServer extends Harness {
  constructor() {
    super({ exe: null });
    this.serverLog = path.join(RUN_DIR, "server.log");
  }

  async start() {
    selfTestSafety();
    if (fs.existsSync(path.join(CLUSTER, "postmaster.pid"))) {
      try {
        pgctl(["--pgdata", CLUSTER, "--mode=fast", "--wait", "stop"]);
      } catch {
        // Stale pid file: nothing is running.
      }
    }
    for (const dir of [CLUSTER, RUN_DIR]) {
      if (!dir.startsWith(`${E2E_ROOT}\\`)) throw new Error(`refusing to clean ${dir}`);
      fs.rmSync(dir, { recursive: true, force: true, maxRetries: 20, retryDelay: 250 });
    }
    fs.mkdirSync(RUN_DIR, { recursive: true });
    execFileSync(path.join(PG_BIN, "initdb.exe"), ["--pgdata", CLUSTER, "--username=postgres", "--auth=trust", "--encoding=UTF8", "--no-locale"], {
      env: pgEnv(),
      stdio: "pipe",
      windowsHide: true,
    });
    this.pgPort = await freePort();
    pgctl(["--pgdata", CLUSTER, `--log=${path.join(RUN_DIR, "postgres.log")}`, `--options=-h 127.0.0.1 -p ${this.pgPort} -c fsync=off -c max_connections=60`, "--wait", "start"]);
    this.clusterStarted = true;

    this.databaseUrl = `postgres://postgres@127.0.0.1:${this.pgPort}/${DATABASE}`;
    const { host, database } = assertLocalTestDatabaseUrl(this.databaseUrl);
    this.dbInfo = { host, database, port: this.pgPort };

    this.emptyEnv = path.join(RUN_DIR, "empty.env");
    fs.writeFileSync(this.emptyEnv, "");
    this.serverPort = await freePort();
    this.apiKey = crypto.randomBytes(24).toString("hex");
    this.bookStorage = path.join(RUN_DIR, "books");
    fs.mkdirSync(this.bookStorage, { recursive: true });

    this.proveSafetyHolds();
    this.runNode(path.join(KOBO, "scripts", "apply-migration.mjs"));

    const postgres = createRequire(path.join(KOBO, "package.json"))("postgres");
    assertLocalTestDatabaseUrl(this.databaseUrl);
    this.sql = postgres(this.databaseUrl, { max: 4, idle_timeout: 5, onnotice: () => {} });

    this.proxy = new E2eProxy({ upstreamPort: this.serverPort });
    await this.startServer();
    await this.proxy.up();
  }

  serverEnv() {
    return {
      ...super.serverEnv(),
      TEMP: E2E_ROOT,
      TMP: E2E_ROOT,
      COVER_STORAGE_DIR: path.join(RUN_DIR, "covers"),
      SUPPORT_STORAGE_DIR: path.join(RUN_DIR, "support"),
    };
  }

  runNode(script) {
    const log = path.join(RUN_DIR, "migration.log");
    execFileSync(process.execPath, ["--import", GUARD, script], { cwd: KOBO, env: this.serverEnv(), stdio: ["ignore", fs.openSync(log, "a"), fs.openSync(log, "a")], windowsHide: true });
  }

  async startServer() {
    const out = fs.openSync(this.serverLog, "a");
    this.serverChild = spawn(process.execPath, ["--import", GUARD, path.join(KOBO, "server", "index.js")], { cwd: KOBO, env: this.serverEnv(), stdio: ["ignore", out, out], windowsHide: true });
    fs.writeFileSync(path.join(RUN_DIR, "server.pid"), String(this.serverChild.pid));
    const child = this.serverChild;
    let exited = null;
    child.once("exit", (code) => {
      exited = code;
    });
    const deadline = Date.now() + 60000;
    for (;;) {
      if (exited !== null) throw new Error(`server exited with ${exited}; see ${this.serverLog}`);
      if (Date.now() > deadline) throw new Error(`server did not start; see ${this.serverLog}`);
      const up = await fetch(`http://127.0.0.1:${this.serverPort}/api/releases/none`).then(
        () => true,
        () => false
      );
      if (up) return;
      await new Promise((r) => setTimeout(r, 100));
    }
  }

  async stop({ keep = false } = {}) {
    await this.proxy?.down().catch(() => {});
    this.proxy?.agent.destroy();
    await this.sql?.end({ timeout: 2 }).catch(() => {});
    await this.stopServer().catch(() => {});
    if (this.clusterStarted) {
      try {
        pgctl(["--pgdata", CLUSTER, "--mode=fast", "--wait", "stop"]);
      } catch (error) {
        process.stderr.write(`could not stop the cluster: ${error.message}\n`);
      }
      this.clusterStarted = false;
    }
    if (!keep) {
      fs.rmSync(CLUSTER, { recursive: true, force: true, maxRetries: 20, retryDelay: 250 });
      fs.rmSync(RUN_DIR, { recursive: true, force: true, maxRetries: 20, retryDelay: 250 });
    }
  }

  // A KOReader client whose pairing does not eat the device's rate-limit allowance.
  async koreader(account, opts = {}) {
    const sim = new KoreaderSim({ baseUrl: this.proxy.url, kobo: KOBO, ...opts });
    this.proxy.freshIp();
    const request = await sim.pairingRequest();
    await this.approve(account, request.user_code);
    await sim.pairingClaim();
    return sim;
  }

  head(md5, userId) {
    return this.sql`
      SELECT ps.device_id, ps.percentage, ps.xpointer, ps.explicit_jump, ps.occurred_at, ps.time_precision, ps.revision, d.name
      FROM device_progress_state ps JOIN devices d ON d.id = ps.device_id
      WHERE ps.book_key = ${`koreader_partial_md5:${md5}`} AND ps.user_id = ${userId}
      ORDER BY CASE WHEN ps.time_precision IN ('exact', 'anchored') THEN ps.occurred_at ELSE ps.received_at END DESC, ps.revision DESC`;
  }

  events(deviceId) {
    return this.sql`
      SELECT client_event_id, client_sequence, event_type, aggregate_id, occurred_at, time_precision, payload, processing_status, rejection_code, server_sequence
      FROM device_events WHERE device_id = ${deviceId} ORDER BY server_sequence`;
  }
}
