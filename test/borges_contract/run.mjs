// End to end contract runner. Exits nonzero on any failed check.
//
//   node test/borges_contract/run.mjs --exe <borges-cli.exe> [--only 1,2] [--keep]

import { Harness } from "./harness.mjs";
import { Firmware, FirmwareProcess } from "./firmware.mjs";
import { SCENARIOS as FIRST } from "./scenarios.mjs";
import { MORE } from "./scenarios_more.mjs";
import { ROUND2 } from "./scenarios_round2.mjs";
import { INBOUND } from "./scenarios_inbound.mjs";

const SCENARIOS = [...FIRST, ...MORE, ...ROUND2, ...INBOUND];

const argv = process.argv.slice(2);
const option = (name) => {
  const i = argv.indexOf(name);
  return i >= 0 ? argv[i + 1] : null;
};
const exe = option("--exe");
if (!exe) {
  console.error("--exe is required");
  process.exit(64);
}
const only = option("--only")?.split(",").map((s) => s.trim());
const keep = argv.includes("--keep");

export class Checks {
  constructor(scenario) {
    this.scenario = scenario;
    this.count = 0;
    this.failures = [];
    this.notes = [];
  }
  ok(condition, message) {
    this.count += 1;
    if (!condition) this.failures.push(message);
    return Boolean(condition);
  }
  eq(actual, expected, message) {
    const same = JSON.stringify(actual) === JSON.stringify(expected);
    this.count += 1;
    if (!same) this.failures.push(`${message}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
    return same;
  }
  near(actual, expected, tolerance, message) {
    this.count += 1;
    const close = Math.abs(Number(actual) - Number(expected)) <= tolerance;
    if (!close) this.failures.push(`${message}: expected about ${expected}, got ${actual}`);
    return close;
  }
  note(text) {
    this.notes.push(text);
  }
}

const harness = new Harness({ exe, keep });
const results = [];
const metrics = {};
let fatal = null;

async function main() {
  await harness.start();
  console.log(`Local throwaway stack up: database ${harness.dbInfo.database} on ${harness.dbInfo.host}:${harness.dbInfo.port}, server on port ${harness.serverPort}`);
  for (const scenario of SCENARIOS) {
    if (only && !only.includes(String(scenario.id))) continue;
    const t = new Checks(scenario);
    const started = Date.now();
    harness.proxy.clear();
    harness.proxy.freshIp();
    let crash = null;
    try {
      await scenario.run(harness, t, metrics);
    } catch (error) {
      crash = error;
      t.failures.push(`threw: ${error.stack ?? error.message}`);
    }
    harness.proxy.clear();
    await harness.proxy.up();
    for (const proc of Firmware.all) proc.kill();
    results.push({ id: scenario.id, name: scenario.name, checks: t.count, failures: t.failures, notes: t.notes, ms: Date.now() - started, crash });
    const line = `${t.failures.length ? "FAIL" : "PASS"}  ${String(scenario.id).padStart(2)}  ${scenario.name}  (${t.count} checks, ${((Date.now() - started) / 1000).toFixed(1)} s)`;
    console.log(line);
    for (const failure of t.failures) console.log(`      x ${failure}`);
    for (const note of t.notes) console.log(`      - ${note}`);
  }
}

let exitCode = 0;
try {
  await main();
} catch (error) {
  fatal = error;
  console.error(`FATAL: ${error.stack ?? error.message}`);
  exitCode = 2;
} finally {
  await harness.stop();
}

console.log("");
console.log("| # | Scenario | Result | Checks | Seconds |");
console.log("|---|---|---|---|---|");
for (const r of results) {
  console.log(`| ${r.id} | ${r.name} | ${r.failures.length ? `FAIL (${r.failures.length})` : "pass"} | ${r.checks} | ${(r.ms / 1000).toFixed(1)} |`);
}
console.log("");
console.log(`Core heap, whole run: peak ${Firmware.heapPeak.peak} bytes during ${Firmware.heapPeak.command}; largest single allocation ${Firmware.heapPeak.largest} bytes; at most ${Firmware.heapPeak.peakOpenFiles} files open at once.`);
for (const [key, value] of Object.entries(metrics)) console.log(`${key}: ${value}`);
if (FirmwareProcess.leakNoted) console.log(`File handle leak: ${FirmwareProcess.leakNoted}`);

const failed = results.filter((r) => r.failures.length);
if (failed.length || fatal || FirmwareProcess.leakNoted) exitCode = exitCode || 1;
if (only === null && results.length !== SCENARIOS.length) exitCode = exitCode || 1;
process.exitCode = exitCode;
