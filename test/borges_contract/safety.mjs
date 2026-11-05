// The one place that decides whether a database may be touched.

export function assertLocalTestDatabaseUrl(url) {
  if (!url) throw new Error("DATABASE_URL is not set");
  let parsed;
  try {
    parsed = new URL(url);
  } catch {
    throw new Error("DATABASE_URL is not a URL");
  }
  if (parsed.hostname !== "127.0.0.1") {
    throw new Error(`database host must be 127.0.0.1, got ${parsed.hostname}`);
  }
  const database = parsed.pathname.replace(/^\//, "");
  if (!database.includes("test")) {
    throw new Error(`database name must contain "test", got "${database}"`);
  }
  return { host: parsed.hostname, port: parsed.port, database };
}

export function isLoopbackHost(host) {
  return host === "127.0.0.1" || host === "::1" || host === "localhost";
}

// Exercised at every start: a harness whose check does not fire is not a check.
export function selfTestSafety() {
  const refused = [
    "postgres://user:secret@db.example.com:5432/app_test",
    "postgres://postgres@127.0.0.1:5432/highlights",
    "postgres://postgres@localhost:5432/app_test",
    "",
  ];
  for (const url of refused) {
    let threw = false;
    try {
      assertLocalTestDatabaseUrl(url);
    } catch {
      threw = true;
    }
    if (!threw) throw new Error(`safety check accepted ${url}`);
  }
  assertLocalTestDatabaseUrl("postgres://postgres@127.0.0.1:5999/borges_contract_test");
}
