// Scenario 15: where a power cut lands, and what a network holding everything back gets called.
// The simulator has no port 80 to ask, so BORGES_SIM_PORTAL stands in for the answer: the reading of
// that answer is tested in borges_net, and what this one proves is the way from there to the screen.

import fs from "node:fs";
import { until } from "./device.mjs";
import { deviceWithBook } from "./scenarios.mjs";
import * as ui from "./ui.mjs";

async function bootLandsOn(d, opts) {
  const boots = d.helloCount;
  const mark = d.mark();
  await d.start(opts);
  await d.waitBoot(boots);
  const seen = await d.waitLog(/Entering activity: (Home|EpubReader)/, { since: mark });
  await d.waitStable(600);
  return seen[1];
}

async function scenario15({ h, t, makeDevice }) {
  const { d } = await deviceWithBook(h, makeDevice, "x4", { prefix: "arranque" });

  // Slept in the book, woke, went Home, power cut: Home is where it was.
  await ui.openFromHome(d);
  await ui.sleepAndWaitFlush(d);
  const woke = await ui.wake(d);
  t.eq(woke.reached, "EpubReader", "waking from a sleep in the book still opens the book");
  await ui.readerToHome(d);
  await d.kill();
  t.eq(await bootLandsOn(d), "Home", "a power cut on Home comes back to Home");
  await d.shot("power-cut-on-home");

  // Asleep in the book when the power goes: the book is where it was. The flush on the way to sleep
  // must not have cleared that.
  await ui.openFromHome(d);
  await ui.sleepAndWaitFlush(d);
  await d.kill();
  t.eq(await bootLandsOn(d), "EpubReader", "a power cut asleep in the book comes back to the book");
  await ui.readerToHome(d);
  await d.kill();

  // Refused, and the network answers port 80 as a portal would.
  await h.proxy.down();
  for (const [language, name] of [["EN", "en"], ["ES", "es"]]) {
    const file = d.file("/.crosspoint/crossink-settings.json");
    const now = fs.existsSync(file) ? JSON.parse(fs.readFileSync(file, "utf8")) : {};
    fs.writeFileSync(file, JSON.stringify({ ...now, language }));
    t.eq(await bootLandsOn(d, { env: { BORGES_SIM_PORTAL: "1" } }), "Home", `boots Home (${name})`);
    await ui.homeToBorges(d);
    const boots = d.helloCount;
    const mark = d.mark();
    await d.press("CONFIRM");
    await d.waitBoot(boots);
    await d.waitLog(/Entering activity: Borges\b/, { since: mark });
    await until("the portal found", () => /asks for a web login/.test(d.logSince(mark)), 30000);
    // The session has still to wind down and say so: same wait as the server-down screen below.
    await new Promise((r) => setTimeout(r, 8000));
    await d.waitStable(500);
    await d.shot(`portal-${name}`);
    t.ok(true, `the sync reached the portal check (${name})`);
    await d.kill();
  }

  // And with no portal the same failure stays what it was.
  t.eq(await bootLandsOn(d), "Home", "boots Home with no portal");
  await ui.homeToBorges(d);
  const boots = d.helloCount;
  const mark = d.mark();
  await d.press("CONFIRM");
  await d.waitBoot(boots);
  await d.waitLog(/Entering activity: Borges\b/, { since: mark });
  await new Promise((r) => setTimeout(r, 8000));
  await d.waitStable(500);
  t.ok(!/asks for a web login/.test(d.logSince(mark)), "a server that is down is not called a portal");
  await d.shot("server-down");
  await d.kill();
  await h.proxy.up();
}

export const BOOT_SCENARIOS = [{ id: 15, name: "Power cuts and captive portals", run: scenario15 }];
