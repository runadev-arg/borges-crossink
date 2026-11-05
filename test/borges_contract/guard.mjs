// Preloaded with `node --import` before scripts/apply-migration.mjs and server/index.js.
// Runs before any of their modules, so nothing can connect anywhere first.
//
// highlights-kobo/.env points to PRODUCTION. Two things keep it out of reach here: the
// process starts with DOTENV_CONFIG_PATH on an empty file and a minimal environment, and
// this guard refuses, process-wide, any socket that does not go to loopback.

import net from "node:net";
import { assertLocalTestDatabaseUrl, isLoopbackHost } from "./safety.mjs";

function die(message) {
  process.stderr.write(`[contract-guard] REFUSING TO RUN: ${message}\n`);
  process.exit(99);
}

if (process.env.BORGES_CONTRACT_HARNESS !== "1") die("not started by the contract harness");
if (!process.env.DOTENV_CONFIG_PATH) die("DOTENV_CONFIG_PATH must point to the harness's empty file");

const expectedUrl = process.env.DATABASE_URL;
try {
  assertLocalTestDatabaseUrl(expectedUrl);
} catch (error) {
  die(error.message);
}

const originalConnect = net.Socket.prototype.connect;
net.Socket.prototype.connect = function guardedConnect(...args) {
  if (process.env.DATABASE_URL !== expectedUrl) die("DATABASE_URL changed after startup");
  const first = Array.isArray(args[0]) ? args[0][0] : args[0];
  const options = typeof first === "object" && first !== null ? first : { port: first, host: args[1] };
  if (!options.path && !isLoopbackHost(options.host ?? "localhost")) {
    die(`outbound connection to ${options.host}:${options.port}`);
  }
  return originalConnect.apply(this, args);
};

// dotenv runs while the main module loads; anything it brought in is caught here.
const check = setInterval(() => {
  if (process.env.DATABASE_URL !== expectedUrl) die("DATABASE_URL changed after startup");
  if (process.env.API_KEY_HETZNER || process.env.SUPABASE_URL || process.env.CONTRA) die("values from .env were loaded");
}, 200);
check.unref();
