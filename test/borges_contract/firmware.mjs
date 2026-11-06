// Drives borges-cli. A FirmwareProcess is one boot of the reader; a Firmware is the
// SD card and the chip, which outlive it.

import { spawn } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import readline from "node:readline";

export class FirmwareProcess {
  constructor(exe, args, env) {
    this.child = spawn(exe, [...args, "--script"], { env, stdio: ["pipe", "pipe", "pipe"], windowsHide: true });
    this.lines = readline.createInterface({ input: this.child.stdout });
    this.waiting = [];
    this.stderr = "";
    this.exited = false;
    this.lines.on("line", (line) => {
      const next = this.waiting.shift();
      if (next) next.resolve(line);
    });
    this.child.stderr.on("data", (d) => {
      this.stderr += d.toString();
    });
    this.child.on("exit", (code) => {
      this.exited = true;
      this.exitCode = code;
      for (const w of this.waiting) w.reject(new Error(`borges-cli exited with ${code}: ${this.stderr}`));
      this.waiting = [];
    });
  }

  async cmd(name, args = {}) {
    if (this.exited) throw new Error(`borges-cli already exited: ${this.exitCode}: ${this.stderr}`);
    const tokens = [name];
    for (const [key, value] of Object.entries(args)) {
      if (value === undefined || value === null) continue;
      const text = String(value);
      if (/[\t\r\n]/.test(text)) throw new Error(`argument ${key} holds a separator`);
      tokens.push(`${key}=${text}`);
    }
    const line = await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error(`borges-cli timed out on ${name}`)), 180000);
      this.waiting.push({
        resolve: (v) => {
          clearTimeout(timer);
          resolve(v);
        },
        reject: (e) => {
          clearTimeout(timer);
          reject(e);
        },
      });
      this.child.stdin.write(`${tokens.join("\t")}\n`);
    });
    const parsed = JSON.parse(line);
    if (parsed.ok !== true) throw new Error(`borges-cli ${name}: ${line}`);
    if (parsed.heap) Firmware.noteHeap(name, parsed.heap, parsed.fs);
    return parsed;
  }

  async close() {
    if (this.exited) return;
    const done = new Promise((resolve) => this.child.once("exit", resolve));
    this.child.stdin.end("exit\n");
    await done;
  }

  kill() {
    if (!this.exited) this.child.kill();
  }
}

export class Firmware {
  static all = new Set();
  static heapPeak = { peak: 0, command: "", largest: 0, peakOpenFiles: 0 };

  static noteHeap(command, heap, files) {
    const h = Firmware.heapPeak;
    if (heap.peak > h.peak) {
      h.peak = heap.peak;
      h.command = command;
    }
    if (heap.largest > h.largest) h.largest = heap.largest;
    if (files?.peak_open > h.peakOpenFiles) h.peakOpenFiles = files.peak_open;
    if (files?.still_open > 0 && !FirmwareProcess.leakNoted) {
      FirmwareProcess.leakNoted = `${command} left ${files.still_open} file(s) open`;
    }
  }

  constructor({ exe, root, name, serverUrl, deviceName = "Xteink X4", options = [] }) {
    this.exe = exe;
    this.name = name;
    this.sd = path.join(root, `sd-${name}`);
    this.serverUrl = serverUrl;
    this.deviceName = deviceName;
    this.mac = crypto.randomBytes(6).toString("hex");
    this.options = options;
    fs.mkdirSync(this.sd, { recursive: true });
    fs.mkdirSync(path.join(this.sd, "books"), { recursive: true });
  }

  // A boot. Options are the CLI's --flags, without the dashes: { "wall-offset": -3600 }.
  boot(options = {}) {
    const args = ["--sd", this.sd, "--mac", this.mac, "--server", this.serverUrl, "--device-name", this.deviceName, ...this.options];
    for (const [key, value] of Object.entries(options)) args.push(`--${key}`, String(value));
    const env = { SystemRoot: process.env.SystemRoot, PATH: process.env.SystemRoot };
    const proc = new FirmwareProcess(this.exe, args, env);
    Firmware.all.add(proc);
    proc.child.once("exit", () => Firmware.all.delete(proc));
    return proc;
  }

  // Boot, one command, power off.
  async once(name, args = {}, options = {}) {
    const proc = this.boot(options);
    try {
      return await proc.cmd(name, args);
    } finally {
      await proc.close();
    }
  }

  putBook(fileName, bytes) {
    const host = path.join(this.sd, "books", fileName);
    fs.writeFileSync(host, bytes);
    return `/books/${fileName}`;
  }

  hostPath(sdPath) {
    return path.join(this.sd, sdPath);
  }

  borgesDir() {
    return path.join(this.sd, ".crosspoint", "borges");
  }

  snapshot(to) {
    fs.rmSync(to, { recursive: true, force: true });
    fs.cpSync(this.borgesDir(), to, { recursive: true });
  }

  restore(from, subset = null) {
    if (!subset) {
      fs.rmSync(this.borgesDir(), { recursive: true, force: true });
      fs.cpSync(from, this.borgesDir(), { recursive: true });
      return;
    }
    for (const item of subset) {
      fs.rmSync(path.join(this.borgesDir(), item), { recursive: true, force: true });
      if (fs.existsSync(path.join(from, item))) fs.cpSync(path.join(from, item), path.join(this.borgesDir(), item), { recursive: true });
    }
  }

  cardFiles(dir = this.sd) {
    return fs.readdirSync(dir, { withFileTypes: true }).flatMap((e) => (e.isDirectory() ? this.cardFiles(path.join(dir, e.name)) : [path.join(dir, e.name)]));
  }

  cardHolds(needle) {
    return this.cardFiles().filter((f) => fs.readFileSync(f).includes(needle));
  }

  fileSize(file) {
    return fs.statSync(file).size;
  }

  // Clips as the reader has them on disk. See TsvClips in borges_cli.cpp.
  writeClips(rows, fileName = "clips.tsv") {
    const esc = (v) => String(v ?? "").replace(/\\/g, "\\\\").replace(/\t/g, "\\t").replace(/\n/g, "\\n");
    const lines = rows.map((c) =>
      [c.spine ?? 0, c.offset ?? 0, c.hash ?? 0, c.bookmark ? 1 : 0, c.text, c.note, c.chapter, c.color, c.xpointer, c.page, c.pctMicro ?? -1, c.timeMs ?? "", c.xpointerEnd ?? ""]
        .map(esc)
        .join("\t")
    );
    const host = path.join(this.sd, fileName);
    fs.writeFileSync(host, `${lines.join("\n")}\n`);
    return host;
  }

  writeUnreadableClips(fileName = "clips.tsv") {
    const host = path.join(this.sd, fileName);
    fs.writeFileSync(host, "UNREADABLE\n");
    return host;
  }
}
