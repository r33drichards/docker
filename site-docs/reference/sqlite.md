<!-- Generated from docs/SQLITE.md; edit that source. -->
# Script SQLite databases

SQLite persists explicitly written data, not a JavaScript heap. Every rehash and
restart creates fresh isolates; globals, closures, authentication and native
capabilities are never restored. This is an opt-in API for trusted scripts, not
an adversarial-code sandbox.

## Configure

Create a directory owned by the daemon user, mode 0700, on persistent storage.
The directory must already exist; it is not automatically provisioned. Relative
paths resolve under the configured config directory.

```xml
<javascriptsettings timeoutms="25" sqlitedirectory="/inspircd/data/script-databases">
<javascript file="scripts/rss.js" authority="user" databases="rss">
```

A grant `rss` permits only `rss.sqlite` inside that root. Grants are independent of
user/admin IRC authority: admin scripts without a grant have no database access.
Directory-loaded scripts inherit their tag's exact grant list. Revocation takes
effect only after a successful rehash: update scripts that still open removed
grants, and verify rehash success. A failed candidate retains old grants and
connections along with the old script set. Sharing a name is
explicit sharing of all tables/data in that database, not row-level isolation.
Use separate database names for different trust domains. No arbitrary paths, URIs,
SQL ATTACH/DETACH, extension loading, virtual tables or script PRAGMAs are exposed.

The host checks private directory ownership, files, links, and SQLite sidecars;
the directory and its ancestors must not be writable by untrusted users. Do not
let another process running as the daemon user modify these paths concurrently.
SQLite is not a defense against a hostile same-UID process or malicious native
code. Use local storage with reliable filesystem locks, not a network filesystem.

## API

```js
const db = ircd.sqlite.open('rss');
let initialized = false;
ircd.on('preCommand', event => {
  if (event.validated) return;
  if (!initialized) {
    db.exec('CREATE TABLE IF NOT EXISTS seen (url TEXT PRIMARY KEY, fetched INTEGER)');
    initialized = true;
  }
  if (event.command !== 'VERSION') return;
  db.transaction(() => {
    db.exec('INSERT OR REPLACE INTO seen(url, fetched) VALUES (?, ?)',
      ['https://example.com/feed', ircd.time()]);
    const result = db.query('SELECT url, fetched FROM seen WHERE url = ?',
      ['https://example.com/feed']);
    // result.columns: ['url', 'fetched']; result.rows: [['https://...', 123]]
  });
});
```

- `open(name)` returns a handle. At most 16 handles may be created during one
  script lifetime, including closed handles; retain and reuse them.
- `query(sql, params = [])` accepts one read-only prepared statement and returns
  `{columns, rows, changes, lastInsertRowid}`. Rows are arrays, preserving duplicate
  column names. No live native statement, cursor or pointer escapes.
- `exec(sql, params = [])` accepts one statement and returns the same shape with
  empty rows. Use this for DDL and writes. Use separate calls for multiple SQL
  statements; text after one statement is rejected.
- Parameters are positional arrays containing null, finite Number, signed 64-bit
  BigInt, string, or Uint8Array. Objects, nested arrays, booleans, undefined and
  unsafe integer Numbers are rejected, not coerced. Use `?` placeholders.
- Integer cells become Numbers when safe, otherwise BigInts; metadata `changes`
  and `lastInsertRowid` are BigInts. Blobs are fresh Uint8Arrays. Text with invalid
  UTF-8 is returned as null; use `CAST(column AS BLOB)` to retrieve arbitrary bytes.
- `transaction(callback)` commits on synchronous success, returns the callback's
  value, and rolls back on a throw, Promise result, or execution termination observed
  before commit starts. Cancellation that arrives during COMMIT/fsync can be too
  late: committed data is not undone even if the surrounding hook times out.
  Nested transactions on the same handle and direct SQL transaction control are
  rejected. There is no transaction spanning separate IRC callbacks/events.
- `close()` releases a connection; closed handles cannot be reused. Closing from
  inside a transaction is rejected. Script disposal closes all remaining handles.

Initialization may open databases and read existing tables but cannot issue SQL
writes or start transactions. Existing WAL-mode databases are rejected, not
converted; this first interface uses rollback-journal mode. SQLite may still
perform its normal crash-recovery I/O while opening an existing database.
Initialize/migrate schemas from an active hook. Opening may
create an empty database even if a later source fails initialization; this is not
an atomic filesystem rehash. A failed rehash retains the old active script set.

Database commits are immediate and independent of the IRC event's queued effects.
A later hook exception, command veto or failure does not undo earlier commits.
Use a database transaction for groups of SQL changes; do not claim a transaction
atomically spans SQLite and IRC/webhook delivery. External API deduplication or
an explicit outbox/retry design is still necessary for reliable feed processing.

## Limits and operations

Operations are synchronous. Lock contention fails immediately (zero busy timeout),
not by waiting on the IRC main loop. Defaults cap SQL at 64 KiB, parameters at 128 and 1 MiB total, columns at 128,
result rows at 1,000 and bytes at 4 MiB, database size at 64 MiB and database
count at 32 per root. SQLite VM work is capped at 100,000 steps and 25 ms
per operation (checked every 100 VM steps). Statements are finalized after each
call; there is no retained statement cache or cursor. These bounds do not bound
filesystem I/O, fsync latency, native allocation or operating-system stalls. Keep
queries small and indexed; benchmark on the deployment storage before enabling a
busy feed. A background-worker API is a possible later addition, not provided by
this synchronous interface. This is not a hard real-time or OOM-isolated service.

SQLite 3.41 or newer is required. The Docker image links the distro SQLite library (`sqlite-dev` at build time,
`sqlite-libs` at runtime). No Node filesystem or npm SQLite package is installed.
There is no Nix build definition in this repository. Persist the whole directory,
including any SQLite journal sidecars, across container replacements. No volume
or production database is provisioned by this change. Stop database writers for
a filesystem-level backup, or use a separately authorized SQLite-aware backup;
do not copy only a live main file and assume it is a consistent backup.
