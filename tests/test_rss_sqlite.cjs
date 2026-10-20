/* SPDX-License-Identifier: MIT */
// Uses real SQLite SQL via Node 24 against the documented embedded contract.
// Native binding and daemon restart are covered separately by integration tests.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const {DatabaseSync} = require('node:sqlite');
const directory = fs.mkdtempSync(path.join(require('node:os').tmpdir(), 'rss-sqlite-test-'));
const databasePath = path.join(directory, 'rss.sqlite');
let sql = new DatabaseSync(databasePath);
let active = false, failure = null;
const bridge = {
  query(statement, parameters = []) {
    assert.match(statement, /^SELECT /, 'query is read-only');
    const prepared = sql.prepare(statement); prepared.setReturnArrays(true); prepared.setReadBigInts(true);
    return {columns: prepared.columns().map(c => c.name), rows: prepared.all(...parameters).map(row => row.map(value => typeof value === 'bigint' && value <= BigInt(Number.MAX_SAFE_INTEGER) && value >= BigInt(Number.MIN_SAFE_INTEGER) ? Number(value) : value)), changes: 0n, lastInsertRowid: 0n};
  },
  exec(statement, parameters = []) {
    assert.ok(active, 'Schema/data writes require active callback');
    if (failure && statement.startsWith(failure)) throw new Error('simulated busy failure');
    const prepared = sql.prepare(statement); prepared.setReadBigInts(true);
    const result = prepared.run(...parameters);
    return {columns: [], rows: [], changes: BigInt(result.changes), lastInsertRowid: BigInt(result.lastInsertRowid)};
  },
  transaction(callback) {
    assert.ok(active, 'Transactions require active callback');
    sql.exec('BEGIN');
    try { const value = callback(); sql.exec('COMMIT'); return value; }
    catch (error) { sql.exec('ROLLBACK'); throw error; }
  },
  close() { sql.close(); },
};
const context = vm.createContext({ircd: {sqlite: {open(name) { assert.equal(name, 'rss'); return bridge; }}}});
vm.runInContext(fs.readFileSync(path.join(__dirname, '../examples/rss.js'), 'utf8').split('// BEGIN RSS SQLITE ADAPTER\n')[1].split('// END RSS SQLITE ADAPTER')[0], context);
const validators = {
  channel: value => /^#[^ ,:\x00-\x1f]+$/.test(value),
  channelKey: value => value.toLowerCase(),
  feed: value => typeof value === 'string' && /^https:\/\/example\.org\/[a-z0-9'_-]*$/.test(value),
};
context.validators = validators;
const store = vm.runInContext('rssSqliteStore(validators)', context);
let slowestCallbackMs = 0;
function callback(fn) {
  active = true; const start = performance.now();
  try { return fn(); } finally { active = false; slowestCallbackMs = Math.max(slowestCallbackMs, performance.now() - start); }
}
assert.equal(store.load().size, 0);
assert.throws(() => store.initialize(), /active callback/);
assert.throws(() => store.add('#rss', 'https://example.org/feed'), /Initialize/);
callback(() => store.initialize()); callback(() => store.initialize());
const feed = "https://example.org/feed'quote";
callback(() => store.add('#RSS', feed));
callback(() => store.add('#rss', feed));
assert.equal(store.load().size, 1);
callback(() => store.remember('#rss', feed, ["id'quoted", 'old'], true));
let loaded = store.load(), sub = [...loaded.values()][0];
assert.equal(sub.seeded, true); assert.equal(sub.job, null); assert.equal(sub.seen.size, 0);
while (!store.restoreSeenPage(sub)) {}
assert.deepEqual([...sub.seen.keys()], ["id'quoted", 'old']);
assert.equal(sub.restoring, false);
// Transaction failure never partially advances the durable baseline or dedup state.
failure = 'UPDATE';
assert.throws(() => callback(() => store.remember('#rss', feed, ['not-committed'], true)), /busy/);
failure = null;
sub = [...store.load().values()][0]; while (!store.restoreSeenPage(sub)) {}
assert.equal(sub.seen.has('not-committed'), false);
// Seen IDs remain bounded and oldest-first in reconstructed Maps.
for (let i = 0; i < 33; ++i) callback(() => store.remember('#rss', feed, Array.from({length: 64}, (_, j) => 'id' + (i * 64 + j))));
sub = [...store.load().values()][0]; let pages = 0;
do { ++pages; } while (!store.restoreSeenPage(sub));
assert.equal(pages, 128); assert.equal(sub.seen.size, 2048);
assert.equal(sub.seen.keys().next().value, 'id64');
assert.equal([...sub.seen.keys()].at(-1), 'id2111');
assert.throws(() => callback(() => store.remember('#rss', feed, Array(65).fill('x'))), /Invalid/);
for (let i = 1; i < 32; ++i) callback(() => store.add('#c' + i, 'https://example.org/feed'));
assert.throws(() => callback(() => store.add('#overflow', 'https://example.org/feed')), /limit/);
// Invalid persisted rows are not scheduled; unknown schema is not reset/destructively migrated.
sql.prepare('UPDATE rss_subscriptions SET feed = ? WHERE channel = ?').run('file:///private', '#c1');
assert.equal(store.load().size, 31);
sql.exec('UPDATE rss_meta SET version = 99');
assert.throws(() => store.load(), /Unsupported/);
assert.throws(() => callback(() => store.initialize()), /Unsupported/);
sql.exec('UPDATE rss_meta SET version = 1');
failure = 'DELETE FROM rss_subscriptions';
assert.throws(() => callback(() => store.remove('#rss', feed)), /busy/);
failure = null;
assert.equal(sql.prepare('SELECT COUNT(*) AS n FROM rss_seen').get().n, 2048);
callback(() => store.remove('#rss', feed));
assert.equal(sql.prepare('SELECT COUNT(*) AS n FROM rss_seen').get().n, 0);
assert.equal(store.load().has('#rss\n' + feed), false);
// Large valid IDs reduce retained history, not consume the 64MiB database cap.
callback(() => store.add('#large', 'https://example.org/large'));
const large = Array.from({length: 64}, (_, i) => String(i).padStart(2, '0') + '界'.repeat(4094));
callback(() => store.remember('#large', 'https://example.org/large', large, true));
sub = store.load().get('#large\nhttps://example.org/large');
while (!store.restoreSeenPage(sub)) {}
assert.ok(sub.seen.size <= Math.floor(131072 / Buffer.byteLength(large[0])));
assert.ok([...sub.seen.keys()].reduce((n, id) => n + Buffer.byteLength(id), 0) <= 131072);
// Full/deadline/busy errors must escape without a partial durable update.
failure = 'INSERT OR IGNORE';
assert.throws(() => callback(() => store.remember('#large', 'https://example.org/large', ['failure'])), /busy/);
failure = null;
// Binding uses BigInt only for integers outside Number's safe range.
sql.exec("UPDATE sqlite_sequence SET seq = 9007199254741000 WHERE name = 'rss_seen'");
callback(() => store.remember('#large', 'https://example.org/large', ['big-sequence']));
sub = store.load().get('#large\nhttps://example.org/large');
while (!store.restoreSeenPage(sub)) {}
assert.equal(sub.seen.has('big-sequence'), true);
const persistedKeys = [...store.load().keys()];
store.close();
// A new database connection and JS realm must reconstruct state without writes.
sql = new DatabaseSync(databasePath);
const fresh = vm.createContext({ircd: context.ircd, validators});
vm.runInContext(fs.readFileSync(path.join(__dirname, '../examples/rss.js'), 'utf8').split('// BEGIN RSS SQLITE ADAPTER\n')[1].split('// END RSS SQLITE ADAPTER')[0], fresh);
const reopened = vm.runInContext('rssSqliteStore(validators)', fresh);
const restored = reopened.load();
assert.deepEqual([...restored.keys()], persistedKeys);
sub = restored.get('#large\nhttps://example.org/large');
assert.equal(sub.seeded, true); assert.equal(sub.job, null); assert.equal(sub.pending.length, 0);
while (!reopened.restoreSeenPage(sub)) {}
assert.equal(sub.seen.has('big-sequence'), true);
assert.equal(sub.seen.has('failure'), false);
reopened.close();
assert.ok(fs.statSync(databasePath).size < 64 * 1024 * 1024);
fs.rmSync(directory, {recursive: true});
console.log('PASS: SQLite adapter, real SQL, read-only startup, versioning, parameter binding, rollback, bounds, BigInt sequences and file-backed fresh-realm restart');

console.log('Observed maximum file-backed callback duration:', slowestCallbackMs.toFixed(2), 'ms (measurement, not a filesystem deadline guarantee)');
