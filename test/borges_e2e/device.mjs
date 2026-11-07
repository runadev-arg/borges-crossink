// One simulated X4: an SD directory that outlives the process, and the process itself.
// Boots are real: a firmware reboot is the simulator relaunching its child, which then
// connects here again. Waiting is on the log, on the frames the panel presents, on files
// and on the server, never on a fixed sleep.

import { spawn, execFileSync } from "node:child_process";
import fs from "node:fs";
import net from "node:net";
import path from "node:path";
import { rawToPng } from "./png.mjs";

// SDL scancodes of the keys the simulator maps to buttons.
const SCANCODES = { BACK: 41, CONFIRM: 40, LEFT: 80, RIGHT: 79, UP: 82, DOWN: 81, POWER: 19, SLEEP: 22 };

const wait = (ms) => new Promise((r) => setTimeout(r, ms));

export async function until(what, fn, timeoutMs = 30000, stepMs = 50) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const value = await fn();
    if (value) return value;
    if (Date.now() > deadline) throw new Error(`timed out after ${timeoutMs} ms waiting for ${what}`);
    await wait(stepMs);
  }
}

export class Device {
  constructor({ name, dir, program, shotsDir, baseUrl, env = {} }) {
    this.name = name;
    this.dir = dir;
    this.sd = path.join(dir, "sd");
    this.temp = path.join(dir, "temp");
    this.logPath = path.join(dir, "simulator.log");
    this.program = program;
    this.shotsDir = shotsDir;
    this.baseUrl = baseUrl;
    this.extraEnv = env;
    this.shotCounter = 0;
    this.child = null;
    this.frames = 0;
    this.frameHash = null;
    this.frameAt = 0;
    this.helloCount = 0;
    this.socket = null;
    this.lastHello = 0;
    this.shotWaiters = new Map();
    this.logOffsetMark = 0;
    fs.mkdirSync(this.sd, { recursive: true });
    fs.mkdirSync(this.temp, { recursive: true });
    fs.mkdirSync(shotsDir, { recursive: true });
  }

  async start({ wakeReason = null, baseUrl = this.baseUrl, env = {} } = {}) {
    if (this.child) throw new Error("already running");
    await this.listen();
    const childEnv = {
      SystemRoot: process.env.SystemRoot,
      PATH: `${path.dirname(this.program)};${process.env.SystemRoot}\\System32`,
      TMP: this.temp,
      TEMP: this.temp,
      SDL_VIDEODRIVER: "dummy",
      CROSSPOINT_SIM_SD: this.sd,
      CROSSPOINT_SIM_HTTP_PORT: String(this.httpPort ?? 0),
      E2E_SIM_CONTROL_PORT: String(this.controlPort),
      ...(baseUrl ? { BORGES_SIM_BASE_URL: baseUrl } : {}),
      ...(wakeReason ? { CROSSPOINT_SIM_WAKE_REASON: wakeReason } : {}),
      ...this.extraEnv,
      ...env,
    };
    this.logFd = fs.openSync(this.logPath, "a");
    this.child = spawn(this.program, [], { cwd: this.dir, env: childEnv, stdio: ["ignore", this.logFd, this.logFd], windowsHide: true });
    this.startedAt = Date.now();
    this.exitInfo = null;
    const child = this.child;
    child.once("exit", (code) => {
      if (this.child === child) {
        this.child = null;
        this.exitInfo = { code };
      }
      try {
        fs.closeSync(this.logFd);
      } catch {
        // Already closed.
      }
    });
  }

  // Per device, so concurrent devices are not an issue: only one process connects at a time.
  async listen() {
    if (this.server) return;
    this.server = net.createServer((socket) => {
      socket.setNoDelay(true);
      socket.setEncoding("latin1");
      let buffer = "";
      socket.on("error", () => {});
      socket.on("data", (data) => {
        buffer += data;
        for (;;) {
          const nl = buffer.indexOf("\n");
          if (nl < 0) break;
          const line = buffer.slice(0, nl);
          buffer = buffer.slice(nl + 1);
          this.onLine(socket, line);
        }
      });
    });
    await new Promise((resolve) => this.server.listen(0, "127.0.0.1", resolve));
    this.controlPort = this.server.address().port;
  }

  onLine(socket, line) {
    const [verb, a, b, c] = line.split(" ");
    if (verb === "HELLO") {
      this.socket = socket;
      this.helloCount += 1;
      this.lastHello = Date.now();
      this.pid = Number(a);
      // A new process has shown nothing yet: an older frame says nothing about this boot.
      this.frameAt = 0;
    } else if (verb === "FRAME") {
      if (b !== this.frameHash) {
        this.frames += 1;
        this.frameHash = b;
      }
      this.frameAt = Date.now();
    } else if (verb === "SHOT") {
      const waiter = this.shotWaiters.get(a);
      if (waiter) {
        this.shotWaiters.delete(a);
        waiter({ width: Number(b), height: Number(c) });
      }
    }
  }

  get running() {
    return this.child !== null;
  }

  log() {
    try {
      return fs.readFileSync(this.logPath, "latin1");
    } catch {
      return "";
    }
  }

  // Everything logged from here on is what waitLog looks at.
  mark() {
    this.logOffsetMark = this.log().length;
    return this.logOffsetMark;
  }

  logSince(offset = this.logOffsetMark) {
    return this.log().slice(offset);
  }

  async waitLog(re, { timeoutMs = 30000, since = this.logOffsetMark } = {}) {
    return until(`log ${re} on ${this.name}`, () => this.logSince(since).match(re), timeoutMs);
  }

  // The next boot of the program, counted by its connecting.
  async waitBoot(afterCount = this.helloCount, timeoutMs = 40000) {
    return until(`boot ${afterCount + 1} of ${this.name}`, () => this.helloCount > afterCount, timeoutMs);
  }

  // The panel kept the same picture for this long. An e-ink frame takes moments to appear.
  async waitStable(quietMs = 700, timeoutMs = 30000) {
    await until(
      `a stable screen on ${this.name}`,
      () => this.frameAt > 0 && Date.now() - this.frameAt >= quietMs && this.socket && !this.socket.destroyed,
      timeoutMs
    );
  }

  async waitNewFrame(afterFrames = this.frames, timeoutMs = 30000) {
    await until(`a new frame on ${this.name}`, () => this.frames > afterFrames, timeoutMs);
  }

  send(line) {
    if (!this.socket || this.socket.destroyed) throw new Error(`no control connection to ${this.name}`);
    this.socket.write(`${line}\n`);
  }

  async press(name, holdMs = 80) {
    const code = SCANCODES[name];
    if (code === undefined) throw new Error(`no such key: ${name}`);
    const before = this.frames;
    this.send(`KEY ${code} ${holdMs}`);
    await wait(holdMs + 60);
    return before;
  }

  // A key press and then the screen settled on something new.
  async tap(name, { holdMs = 80, settleMs = 500, timeoutMs = 15000, needFrame = true } = {}) {
    const before = await this.press(name, holdMs);
    if (needFrame) await this.waitNewFrame(before, timeoutMs);
    await this.waitStable(settleMs, timeoutMs);
  }

  async taps(names, opts) {
    for (const name of names) await this.tap(name, opts);
  }

  async shot(label) {
    this.shotCounter += 1;
    const id = String(this.shotCounter);
    const base = `${String(this.shotCounter).padStart(3, "0")}-${label}`;
    const raw = path.join(this.shotsDir, `${base}.raw`);
    const reply = new Promise((resolve) => this.shotWaiters.set(id, resolve));
    this.send(`SHOT ${id} ${raw}`);
    const result = await Promise.race([reply, wait(15000).then(() => null)]);
    if (!result || !result.width) throw new Error(`screenshot ${label} failed on ${this.name}`);
    const png = path.join(this.shotsDir, `${base}.png`);
    rawToPng(raw, result.width, result.height, png);
    return png;
  }

  // The presented frame as gray bytes, for the few checks a pixel can answer. A person
  // looks at the screenshots; this only tells the harness where it is.
  async pixels() {
    this.shotCounter += 1;
    const id = `p${this.shotCounter}`;
    const raw = path.join(this.temp, `${id}.raw`);
    const reply = new Promise((resolve) => this.shotWaiters.set(id, resolve));
    this.send(`SHOT ${id} ${raw}`);
    const result = await Promise.race([reply, wait(15000).then(() => null)]);
    if (!result || !result.width) throw new Error(`could not read the screen of ${this.name}`);
    const bytes = fs.readFileSync(raw);
    fs.rmSync(raw, { force: true });
    return {
      width: result.width,
      height: result.height,
      // Runs of solid black at least this long, row by row.
      solidRuns: (minimum, light = false) => {
        const runs = [];
        for (let y = 0; y < result.height; y += 1) {
          let start = -1;
          for (let x = 0; x <= result.width; x += 1) {
            const value = x < result.width ? bytes[(y * result.width + x) * 4 + 1] : -1;
            const dark = value >= 0 && (light ? value > 215 : value < 40);
            if (dark && start < 0) start = x;
            if (!dark && start >= 0) {
              if (x - start >= minimum) runs.push({ y, x0: start, x1: x });
              start = -1;
            }
          }
        }
        return runs;
      },
      // Not white: covers both a solid fill and a dither averaged by the window's scaling.
      inkFraction: (x0, y0, x1, y1) => {
        let ink = 0;
        let total = 0;
        for (let y = y0; y < y1; y += 1) {
          for (let x = x0; x < x1; x += 1) {
            total += 1;
            if (bytes[(y * result.width + x) * 4 + 1] < 235) ink += 1;
          }
        }
        return ink / total;
      },
      darkFraction: (x0, y0, x1, y1) => {
        let dark = 0;
        let total = 0;
        for (let y = y0; y < y1; y += 1) {
          for (let x = x0; x < x1; x += 1) {
            total += 1;
            if (bytes[(y * result.width + x) * 4 + 1] < 128) dark += 1;
          }
        }
        return dark / total;
      },
    };
  }

  // Power cut: everything dies now, no flush, as when a battery comes out.
  async kill() {
    if (!this.child) return;
    const child = this.child;
    this.child = null;
    const gone = new Promise((resolve) => child.once("exit", resolve));
    try {
      execFileSync("taskkill.exe", ["/PID", String(child.pid), "/T", "/F"], { stdio: "ignore", windowsHide: true });
    } catch {
      // Already gone.
    }
    await Promise.race([gone, wait(5000)]);
    if (this.socket) this.socket.destroy();
    this.socket = null;
    this.frameAt = 0;
  }

  // Kills only the running child, so the supervisor sees it die: same result as above but
  // from inside a boot, for the crash-loop tests.
  async quit() {
    if (!this.child) return;
    const child = this.child;
    const gone = new Promise((resolve) => child.once("exit", resolve));
    try {
      this.send("QUIT");
    } catch {
      await this.kill();
      return;
    }
    const ended = await Promise.race([gone.then(() => true), wait(5000).then(() => false)]);
    if (!ended) await this.kill();
    this.child = null;
    if (this.socket) this.socket.destroy();
    this.socket = null;
    this.frameAt = 0;
  }

  async close() {
    await this.kill();
    if (this.server) {
      const server = this.server;
      this.server = null;
      await new Promise((resolve) => server.close(resolve));
    }
  }

  file(rel) {
    return path.join(this.sd, rel.replace(/^\//, ""));
  }

  // Every file on this card and in this device's temp and logs, for leak checks.
  allFiles() {
    const out = [];
    const walk = (dir) => {
      let entries = [];
      try {
        entries = fs.readdirSync(dir, { withFileTypes: true });
      } catch {
        return;
      }
      for (const e of entries) {
        const full = path.join(dir, e.name);
        if (e.isDirectory()) walk(full);
        else out.push(full);
      }
    };
    walk(this.dir);
    return out;
  }

  // The cache directory the firmware keeps for a book, by looking for its progress.bin.
  progressFiles() {
    return this.allFiles().filter((f) => f.endsWith("progress.bin"));
  }
}
