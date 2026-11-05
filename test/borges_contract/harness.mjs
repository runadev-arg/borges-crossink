// Boots PostgreSQL 17 and the real Borges server on throwaway storage, and offers the
// account-side operations a person would do in the web app.
//
// Only processes started here are ever stopped: the server child, and the cluster in
// its one fixed directory.

import { execFileSync, spawn } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import net from "node:net";
import path from "node:path";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import { FaultProxy } from "./proxy.mjs";
import { Firmware } from "./firmware.mjs";
import { KoreaderSim } from "./koreader.mjs";
import { assertLocalTestDatabaseUrl, selfTestSafety } from "./safety.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));

export const KOBO = "C:\\Users\\totot\\Documents\\Proyectos\\highlights-kobo";
export const ROOT = "D:\\CrossInkBorges\\tmp";
const PG_BIN = "C:\\Program Files\\PostgreSQL\\17\\bin";
export const CLUSTER = path.join(ROOT, "contract-pg");
export const RUN_DIR = path.join(ROOT, "contract-run");
const DATABASE = "borges_contract_test";

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

function pgEnv() {
  return {
    SystemRoot: process.env.SystemRoot,
    PATH: `${PG_BIN};${process.env.SystemRoot}\\System32`,
    TEMP: ROOT,
    TMP: ROOT,
    PGCLIENTENCODING: "UTF8",
  };
}

function pgctl(args) {
  // stdio ignored, not piped: the postmaster inherits the handles and would hold a pipe open forever.
  return execFileSync(path.join(PG_BIN, "pg_ctl.exe"), args, { env: pgEnv(), stdio: "ignore", windowsHide: true });
}

function stopLeftoverCluster() {
  if (fs.existsSync(path.join(CLUSTER, "postmaster.pid"))) {
    try {
      pgctl(["--pgdata", CLUSTER, "--mode=fast", "--wait", "stop"]);
    } catch {
      // Stale pid file: nothing is running.
    }
  }
}

export class Harness {
  constructor({ exe, keep = false }) {
    this.exe = exe;
    this.keep = keep;
    this.serverChild = null;
    this.serverLog = path.join(RUN_DIR, "server.log");
    this.counter = 0;
    this.ipCounter = 1;
    this.deviceSeq = 0;
  }

  // ---------------------------------------------------------------------------------
  // Lifecycle
  // ---------------------------------------------------------------------------------

  async start() {
    selfTestSafety();
    stopLeftoverCluster();
    for (const dir of [CLUSTER, RUN_DIR]) {
      if (!dir.startsWith(`${ROOT}\\`)) throw new Error(`refusing to clean ${dir}`);
      fs.rmSync(dir, { recursive: true, force: true });
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

    this.proxy = new FaultProxy({ upstreamPort: this.serverPort });
    await this.startServer();
    await this.proxy.up();
  }

  serverEnv() {
    assertLocalTestDatabaseUrl(this.databaseUrl);
    // A closed list. Nothing is inherited, so nothing from the machine or from .env can
    // change where this server talks to.
    return {
      SystemRoot: process.env.SystemRoot,
      PATH: `${path.dirname(process.execPath)};${process.env.SystemRoot}\\System32`,
      TEMP: ROOT,
      TMP: ROOT,
      NODE_ENV: "development",
      BORGES_CONTRACT_HARNESS: "1",
      DOTENV_CONFIG_PATH: this.emptyEnv,
      DOTENV_CONFIG_QUIET: "true",
      DATABASE_URL: this.databaseUrl,
      PORT: String(this.serverPort),
      TRUST_PROXY_HOPS: "1",
      SYNC_API_KEY: this.apiKey,
      SUPPORT_HASH_SECRET: "contract-only",
      BOOK_STORAGE_DIR: this.bookStorage,
      COVER_STORAGE_DIR: path.join(RUN_DIR, "covers"),
      SUPPORT_STORAGE_DIR: path.join(RUN_DIR, "support"),
      COVER_PROVIDER_ENABLED: "0",
      SUPPORT_ATTACHMENTS_ENABLED: "0",
      LEGACY_DEVICE_API_KEY_ENABLED: "0",
      LIBRARY_MIN_FREE_DISK_BYTES: "1000000",
      PUBLIC_BASE_URL: `http://127.0.0.1:${this.serverPort}`,
    };
  }

  // Before anything of the server runs: from its own directory, with the real dotenv and
  // the real .env next to it, that file stays out, and the guard does its job. No value
  // is printed and no packet leaves: the guard dies before the socket exists.
  proveSafetyHolds() {
    const guard = path.join(HERE, "guard.mjs").replace(/\\/g, "/").replace(/^([A-Za-z]):/, "file:///$1:");
    const probe = (env, code) => {
      try {
        execFileSync(process.execPath, ["--import", guard, "--input-type=module", "-e", code], { cwd: KOBO, env, stdio: "ignore", windowsHide: true });
        return 0;
      } catch (error) {
        return error.status;
      }
    };
    const env = this.serverEnv();
    const dotenvStaysOut = `await import("dotenv/config");
      process.exit(process.env.DATABASE_URL === ${JSON.stringify(this.databaseUrl)} && !("SUPABASE_URL" in process.env) && !("API_KEY_HETZNER" in process.env) && !("GEMINI_API_KEY" in process.env) ? 0 : 3);`;
    if (probe(env, dotenvStaysOut) !== 0) throw new Error("safety: dotenv loaded values from the repository's .env");
    if (probe(env, `(await import("node:net")).connect(443, "203.0.113.1"); setTimeout(() => process.exit(0), 200);`) !== 99) {
      throw new Error("safety: the guard allowed a non-loopback connection");
    }
    if (probe({ ...env, DATABASE_URL: "postgres://u:p@db.example.com:5432/app_test" }, "process.exit(0);") !== 99) {
      throw new Error("safety: the guard accepted a remote database");
    }
    if (probe({ ...env, DATABASE_URL: `postgres://postgres@127.0.0.1:${this.pgPort}/produccion` }, "process.exit(0);") !== 99) {
      throw new Error("safety: the guard accepted a database not named test");
    }
  }

  runNode(script) {
    execFileSync(process.execPath, ["--import", path.join(HERE, "guard.mjs").replace(/\\/g, "/").replace(/^([A-Za-z]):/, "file:///$1:"), script], {
      cwd: KOBO,
      env: this.serverEnv(),
      stdio: ["ignore", fs.openSync(path.join(RUN_DIR, "migration.log"), "a"), fs.openSync(path.join(RUN_DIR, "migration.log"), "a")],
      windowsHide: true,
    });
  }

  async startServer() {
    const out = fs.openSync(this.serverLog, "a");
    // cwd is the repository because static paths are relative to it. dotenv reads
    // DOTENV_CONFIG_PATH, not ./.env, and the guard dies if that ever stops being true.
    this.serverChild = spawn(
      process.execPath,
      ["--import", path.join(HERE, "guard.mjs").replace(/\\/g, "/").replace(/^([A-Za-z]):/, "file:///$1:"), path.join(KOBO, "server", "index.js")],
      { cwd: KOBO, env: this.serverEnv(), stdio: ["ignore", out, out], windowsHide: true }
    );
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

  async stopServer() {
    if (!this.serverChild) return;
    const child = this.serverChild;
    this.serverChild = null;
    if (child.exitCode !== null) return;
    const gone = new Promise((resolve) => child.once("exit", resolve));
    child.kill();
    await gone;
    this.proxy.agent.destroy();
  }

  async stop() {
    for (const proc of Firmware.all) proc.kill();
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
    if (!this.keep) {
      fs.rmSync(CLUSTER, { recursive: true, force: true });
      fs.rmSync(RUN_DIR, { recursive: true, force: true });
    }
  }

  // ---------------------------------------------------------------------------------
  // Account side: what a person does in the web app. Direct to the server, each call
  // from its own IP so these never eat a rate limit a test is measuring.
  // ---------------------------------------------------------------------------------

  webIp() {
    this.ipCounter += 1;
    return `192.168.${(this.ipCounter >> 8) & 255}.${this.ipCounter & 255}`;
  }

  async web(method, url, { cookie, body, form } = {}) {
    const headers = { "x-forwarded-for": this.webIp() };
    if (cookie) headers.cookie = cookie;
    let payload;
    if (form) {
      payload = form;
    } else if (body !== undefined) {
      headers["content-type"] = "application/json";
      payload = JSON.stringify(body);
    }
    const res = await fetch(`http://127.0.0.1:${this.serverPort}${url}`, { method, headers, body: payload });
    const text = await res.text();
    let parsed = null;
    try {
      parsed = text ? JSON.parse(text) : null;
    } catch {
      parsed = text;
    }
    return { status: res.status, body: parsed, headers: res.headers };
  }

  async account(prefix = "lector") {
    this.counter += 1;
    const username = `${prefix}${this.counter}${crypto.randomBytes(3).toString("hex")}`;
    const res = await this.web("POST", "/api/accounts/register", {
      body: { username, email: `${username}@contract.invalid`, password: `Contrato-${crypto.randomBytes(8).toString("hex")}!` },
    });
    if (res.status !== 201) throw new Error(`register failed: ${res.status} ${JSON.stringify(res.body)}`);
    const setCookie = res.headers.get("set-cookie");
    const cookie = setCookie.split(";")[0];
    return { username, cookie, id: res.body.user.id };
  }

  async approve(account, userCode, scopes) {
    const res = await this.web("POST", "/api/devices/pairing/approve", {
      cookie: account.cookie,
      body: { user_code: userCode, ...(scopes ? { scopes } : {}) },
    });
    if (res.status !== 200) throw new Error(`approve failed: ${res.status} ${JSON.stringify(res.body)}`);
    return res.body;
  }

  async reject(account, userCode) {
    return this.web("POST", "/api/devices/pairing/reject", { cookie: account.cookie, body: { user_code: userCode } });
  }

  async revoke(account, deviceId) {
    return this.web("POST", `/api/devices/${deviceId}/revoke`, { cookie: account.cookie, body: {} });
  }

  async upload(account, fileName, bytes) {
    const form = new FormData();
    form.append("book", new Blob([bytes], { type: "application/epub+zip" }), fileName);
    const res = await this.web("POST", "/api/library/upload", { cookie: account.cookie, form });
    if (res.status >= 300) throw new Error(`upload of ${fileName} failed: ${res.status} ${JSON.stringify(res.body)}`);
    return res.body;
  }

  // ---------------------------------------------------------------------------------
  // Clients
  // ---------------------------------------------------------------------------------

  firmware(name = null, opts = {}) {
    this.deviceSeq += 1;
    return new Firmware({ exe: this.exe, root: RUN_DIR, name: name ?? `fw${this.deviceSeq}`, serverUrl: this.proxy.url, ...opts });
  }

  async pairFirmware(fw, account) {
    const proc = fw.boot();
    try {
      this.proxy.freshIp();
      const started = await proc.cmd("pair-start");
      if (started.state !== "waiting") throw new Error(`pair-start: ${JSON.stringify(started)}`);
      await this.approve(account, started.user_code);
      const done = await proc.cmd("pair-poll");
      if (done.state !== "done") throw new Error(`pair-poll: ${JSON.stringify(done)}`);
      fw.deviceId = done.device_id;
      return done;
    } finally {
      await proc.close();
    }
  }

  async koreader(account, opts = {}) {
    const sim = new KoreaderSim({ baseUrl: this.proxy.url, kobo: KOBO, ...opts });
    this.proxy.freshIp();
    const request = await sim.pairingRequest();
    await this.approve(account, request.user_code);
    await sim.pairingClaim();
    return sim;
  }

  async fwAndKoreader(prefix = "lector") {
    const account = await this.account(prefix);
    const fw = this.firmware();
    await this.pairFirmware(fw, account);
    const kr = await this.koreader(account);
    return { account, fw, kr };
  }
}
