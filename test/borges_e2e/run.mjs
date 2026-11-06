// node test/borges_e2e/run.mjs --program <program.exe> [--only 1,2] [--keep]
//
// Everything lives under D:\CrossInkBorges\tmp\e2e. The final screenshots of a run are moved
// to D:\CrossInkBorges\artifacts\e2e, where a person has to look at them: pixels this harness
// only uses to find its way, it cannot read a screen.

import fs from "node:fs";
import path from "node:path";
import { Device } from "./device.mjs";
import { E2eServer, E2E_ROOT } from "./server.mjs";
import { SCENARIOS } from "./scenarios.mjs";
import { MORE_SCENARIOS } from "./scenarios_more.mjs";
import { INBOUND_SCENARIOS } from "./scenarios_inbound.mjs";
import { BOOT_SCENARIOS } from "./scenarios_boot.mjs";

const ARTIFACTS = "D:\\CrossInkBorges\\artifacts\\e2e";
const args = process.argv.slice(2);
const option = (name) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : null;
};
const program = option("--program");
const keep = args.includes("--keep");
const only = option("--only")?.split(",").map((s) => s.trim());
if (!program) throw new Error("--program is required");

class Checks {
  constructor() {
    this.count = 0;
    this.failures = [];
    this.notes = [];
    this.notExercised = [];
  }
  ok(value, message) {
    this.count += 1;
    if (!value) this.failures.push(message);
    return Boolean(value);
  }
  eq(actual, expected, message) {
    return this.ok(actual === expected, `${message}: got ${JSON.stringify(actual)}, wanted ${JSON.stringify(expected)}`);
  }
  near(actual, expected, tolerance, message) {
    return this.ok(Math.abs(actual - expected) <= tolerance, `${message}: got ${actual}, wanted ${expected} +/- ${tolerance}`);
  }
  note(text) {
    this.notes.push(text);
  }
  skip(text) {
    this.notExercised.push(text);
  }
}

// Windows can hold a just-emptied directory for a moment after a process that had it as its
// working directory died. An empty directory left behind is not worth failing a run: the
// next start clears it, or the end of this run does.
function removeQuietly(dir) {
  try {
    fs.rmSync(dir, { recursive: true, force: true, maxRetries: 8, retryDelay: 100 });
  } catch (error) {
    console.log(`note: could not remove ${dir} yet (${error.code})`);
  }
}

const results = [];
const harness = new E2eServer();
const shotsRoot = path.join(E2E_ROOT, "shots");
const devicesRoot = path.join(E2E_ROOT, "devices");
let exitCode = 0;

async function main() {
  for (const dir of [shotsRoot, devicesRoot]) removeQuietly(dir);
  await harness.start();
  console.log(`Local throwaway stack up: database ${harness.dbInfo.database} on ${harness.dbInfo.host}:${harness.dbInfo.port}, server on port ${harness.serverPort}`);
  for (const scenario of [...SCENARIOS, ...MORE_SCENARIOS, ...INBOUND_SCENARIOS, ...BOOT_SCENARIOS]) {
    if (only && !only.includes(String(scenario.id))) continue;
    const t = new Checks();
    const devices = [];
    const makeDevice = (name = "x4", opts = {}) => {
      const device = new Device({
        name,
        dir: path.join(devicesRoot, `s${scenario.id}-${name}`),
        program,
        shotsDir: path.join(shotsRoot, `s${String(scenario.id).padStart(2, "0")}`),
        baseUrl: harness.proxy.url,
        ...opts,
      });
      devices.push(device);
      return device;
    };
    const started = Date.now();
    harness.proxy.clear();
    harness.proxy.freshIp();
    await harness.proxy.up();
    try {
      await scenario.run({ h: harness, t, makeDevice });
    } catch (error) {
      t.failures.push(`threw: ${error.stack ?? error.message}`);
    }
    harness.proxy.clear();
    await harness.proxy.up();
    for (const device of devices) {
      await device.close().catch(() => {});
      if (!keep) removeQuietly(device.dir);
    }
    const na = t.count === 0 && t.failures.length === 0;
    const verdict = t.failures.length ? "FAIL" : na ? "N/A" : "PASS";
    results.push({ id: scenario.id, name: scenario.name, verdict, checks: t.count, failures: t.failures, notes: t.notes, notExercised: t.notExercised, seconds: (Date.now() - started) / 1000 });
    console.log(`${verdict}  ${String(scenario.id).padStart(2)}  ${scenario.name}  (${t.count} checks, ${((Date.now() - started) / 1000).toFixed(1)} s)`);
    for (const failure of t.failures) console.log(`      x ${failure}`);
    for (const note of t.notes) console.log(`      - ${note}`);
    for (const skip of t.notExercised) console.log(`      ~ not exercised: ${skip}`);
  }
}

try {
  await main();
} catch (error) {
  console.error(`FATAL: ${error.stack ?? error.message}`);
  exitCode = 2;
} finally {
  await harness.stop({ keep }).catch((error) => console.error(`stop: ${error.message}`));
}

console.log("");
console.log("| # | Scenario | Result | Checks | Seconds |");
console.log("|---|---|---|---|---|");
for (const r of results) console.log(`| ${r.id} | ${r.name} | ${r.verdict} | ${r.checks} | ${r.seconds.toFixed(1)} |`);

if (fs.existsSync(shotsRoot)) {
  fs.mkdirSync(ARTIFACTS, { recursive: true });
  for (const entry of fs.readdirSync(shotsRoot)) {
    const target = path.join(ARTIFACTS, entry);
    fs.rmSync(target, { recursive: true, force: true });
    fs.cpSync(path.join(shotsRoot, entry), target, { recursive: true });
  }
  fs.writeFileSync(path.join(ARTIFACTS, only ? `results-${only.join("_")}.json` : "results.json"), JSON.stringify(results, null, 2));
  if (!keep) removeQuietly(shotsRoot);
}
if (!keep) removeQuietly(devicesRoot);
if (results.some((r) => r.verdict === "FAIL")) exitCode = exitCode || 1;
process.exit(exitCode);
