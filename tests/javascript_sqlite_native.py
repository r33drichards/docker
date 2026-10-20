#!/usr/bin/env python3
"""Loopback integration against the actual compiled SQLite JavaScript module.

Run INSPIRCD_NATIVE=/path/to/native/run tests/javascript_sqlite.sh. All IRC
traffic, configuration, JavaScript, and database files are synthetic and local.
The standard-library sqlite3 connection is used only to verify durable bytes and
released locks, never as an implementation substitute for ircd.sqlite.
"""

import contextlib
import grp
import os
import pathlib
import pwd
import signal
import socket
import sqlite3
import statistics
import subprocess
import tempfile
import time

from precommand_client import Client

RUN = pathlib.Path(os.environ["INSPIRCD_NATIVE"]).resolve()
PROBE = """
ircd.on('preCommand', e => {
  if (!e.validated && e.command === 'XSQLITE')
    ircd.notice(e.userId, 'sqlite:probe:' +
      ((ircd.sqlite && typeof ircd.sqlite.open === 'function') ? 'ok' : 'MISSING_NATIVE_API'));
});
"""


# Each assertion runs inside the V8 isolate loaded by m_javascript.so. Database
# operations here are the public JS host API, not Python SQL stand-ins.
SUITE = r"""
function check(ok, what) { if (!ok) throw Error(what); }
function rejected(fn, what) {
  let threw = false;
  try { fn(); } catch (_) { threw = true; }
  check(threw, 'accepted forbidden operation: ' + what);
}
function resultShape(r) {
  check(Array.isArray(r.columns) && Array.isArray(r.rows), 'result arrays');
  check(typeof r.changes === 'bigint' && typeof r.lastInsertRowid === 'bigint', 'metadata bigint');
}
const db = ircd.sqlite.open('rss');
check(db.query('SELECT 42 AS answer').rows[0][0] === 42, 'initialization read');
rejected(() => db.exec('CREATE TABLE forbidden_init (value INTEGER)'), 'initialization write');
rejected(() => db.query('CREATE TABLE forbidden_query (value INTEGER)'), 'query write at initialization');
rejected(() => db.transaction(() => {}), 'initialization transaction');
let initDurable = null;
try { initDurable = db.query("SELECT value FROM state WHERE key='durable'").rows[0][0]; } catch (_) {}
let heap = 0;
function count(label) { return db.query('SELECT count(*) FROM changes WHERE label=?', [label]).rows[0][0]; }
const cases = {
  initialize() {
    resultShape(db.exec('CREATE TABLE IF NOT EXISTS state (key TEXT PRIMARY KEY, value INTEGER)'));
    db.exec('CREATE TABLE IF NOT EXISTS changes (id INTEGER PRIMARY KEY, label TEXT UNIQUE)');
    db.exec('CREATE TABLE IF NOT EXISTS bindings (id INTEGER PRIMARY KEY, text_value TEXT, blob_value BLOB, null_value, int_value INTEGER, float_value REAL)');
    db.exec("INSERT OR REPLACE INTO state VALUES('durable', 73)");
    check(db.query("SELECT count(*) FROM sqlite_master WHERE name IN ('forbidden_init','forbidden_query')").rows[0][0] === 0, 'init write changed schema');
    rejected(() => db.query("INSERT INTO changes(label) VALUES('query-write')"), 'query must remain read-only');
    check(count('query-write') === 0, 'query wrote data');
  },
  binds() {
    const text = "caf\u00e9 '); DROP TABLE bindings; --\u0000tail";
    const original = new Uint8Array([99, 0, 128, 255, 88]);
    const slice = original.subarray(1, 4);
    const written = db.exec('INSERT INTO bindings(text_value,blob_value,null_value,int_value,float_value) VALUES(?,?,?,?,?)', [text, slice, null, 9223372036854775807n, 1.25]);
    resultShape(written);
    check(written.rows.length === 0 && written.changes === 1n && written.lastInsertRowid === 1n, 'exec metadata');
    original.fill(7);
    const read = db.query('SELECT text_value,blob_value,null_value,int_value,float_value FROM bindings WHERE id=?', [1]);
    resultShape(read);
    check(read.columns.join(',') === 'text_value,blob_value,null_value,int_value,float_value', 'column order');
    const row = read.rows[0];
    check(row[0] === text && row[1] instanceof Uint8Array && Array.from(row[1]).join(',') === '0,128,255', 'bound text/blob');
    check(row[2] === null && row[3] === 9223372036854775807n && row[4] === 1.25, 'bound null/wide integer/real');
    row[1][0] = 42;
    check(db.query('SELECT blob_value FROM bindings').rows[0][0][0] === 0, 'result blob ownership');
    const limits = db.query('SELECT ?,?,?,?,?,?', [-9223372036854775808n, 9007199254740991n, -9007199254740991n, 9007199254740992n, -9007199254740992n, new Uint8Array(0)]).rows[0];
    check(limits[0] === -9223372036854775808n && limits[1] === 9007199254740991 && limits[2] === -9007199254740991, 'integer result conversion');
    check(limits[3] === 9007199254740992n && limits[4] === -9007199254740992n && limits[5] instanceof Uint8Array && limits[5].length === 0, 'wide result and empty blob');
    const nul = db.query('SELECT ? AS empty, ? AS nil', ['', null]);
    check(nul.rows[0][0] === '' && nul.rows[0][1] === null, 'empty is not null');
  },
  commit() {
    const returned = db.transaction(() => {
      db.exec('INSERT INTO changes(label) VALUES(?)', ['committed']);
      check(count('committed') === 1, 'transaction read-your-writes');
      return 17;
    });
    check(returned === 17 && count('committed') === 1, 'transaction commit/return');
  },
  rollback() {
    rejected(() => db.transaction(() => {
      db.exec('INSERT INTO changes(label) VALUES(?)', ['sql-rollback']);
      db.exec('INSERT INTO table_that_does_not_exist VALUES(1)');
    }), 'SQL error');
    check(count('sql-rollback') === 0, 'SQL error rollback');
    const sentinel = new Error('synthetic callback failure');
    let caught;
    try {
      db.transaction(() => { db.exec("INSERT INTO changes(label) VALUES('js-rollback')"); throw sentinel; });
    } catch (e) { caught = e; }
    check(caught === sentinel && count('js-rollback') === 0, 'original callback error and rollback');
    // A failed transaction must not strand its connection in BEGIN.
    db.transaction(() => db.exec("INSERT INTO changes(label) VALUES('after-rollback')"));
    check(count('after-rollback') === 1, 'connection after rollback');
  },
  autoabort() {
    rejected(() => db.transaction(() => {
      db.exec("INSERT INTO changes(label) VALUES('autoabort-before')");
      try { db.exec("INSERT OR ROLLBACK INTO changes(label) VALUES('committed')"); } catch (_) {}
      db.exec("INSERT INTO changes(label) VALUES('autoabort-after')");
    }), 'SQL-autoaborted transaction must fail');
    check(count('autoabort-before') === 0 && count('autoabort-after') === 0, 'caught conflict allowed escaped commit');
    db.transaction(() => check(count('committed') === 1, 'connection after SQL autoabort'));
  },
  synchronous() {
    rejected(() => db.transaction(() => {
      db.exec("INSERT INTO changes(label) VALUES('promise-rollback')");
      return Promise.resolve('not synchronous');
    }), 'Promise transaction');
    check(count('promise-rollback') === 0, 'Promise rollback');
    rejected(() => db.transaction(() => {
      db.exec("INSERT INTO changes(label) VALUES('nested-rollback')");
      db.transaction(() => db.exec("INSERT INTO changes(label) VALUES('nested-inner')"));
    }), 'nested transaction');
    check(count('nested-rollback') === 0 && count('nested-inner') === 0, 'nested rollback');
    rejected(() => db.transaction('not a callback'), 'non-callback');
    rejected(() => db.transaction(() => db.close()), 'close in transaction');
    check(db.query('SELECT 1').rows[0][0] === 1, 'connection after close rejection');
  },
  malformed() {
    for (const value of [undefined, true, {}, [], new Int8Array([1]), NaN, Infinity, -Infinity, 9007199254740992, 9223372036854775808n, -9223372036854775809n])
      rejected(() => db.query('SELECT ?', [value]), 'bind type/range');
    rejected(() => db.query('SELECT ?', []), 'missing bind');
    rejected(() => db.query('SELECT 1', [1]), 'extra bind');
    rejected(() => db.query('SELECT ?', new Array(1)), 'sparse bind');
    rejected(() => db.query('SELECT ?', {0: 1, length: 1}), 'non-array binds');
    rejected(() => db.query('SELECT ?', undefined), 'explicit undefined parameters');
    rejected(() => db.query(1), 'non-string SQL');
    rejected(() => db.query('SELECT 1', [], 3), 'excess arguments');
    rejected(() => db.query(''), 'empty SQL');
    rejected(() => db.query('SELECT 1\u0000; SELECT 2'), 'NUL SQL');
    rejected(() => db.query('SELECT ' + '?'.repeat(65537)), 'oversized SQL');
    rejected(() => db.query('SELECT ?', [new Uint8Array(1024*1024+1)]), 'oversized blob');
    rejected(() => db.query('SELECT ?', ['x'.repeat(1024*1024+1)]), 'oversized text');
    rejected(() => db.query('SELECT ?', Array(129).fill(0)), 'excess parameters');
    check(db.query('SELECT 5').rows[0][0] === 5, 'connection after invalid inputs');
  },
  limits() {
    rejected(() => db.query('WITH RECURSIVE n(x) AS (VALUES(0) UNION ALL SELECT x+1 FROM n WHERE x<1100) SELECT x FROM n'), 'result row quota');
    rejected(() => db.query('SELECT zeroblob(1048577)'), 'result cell quota');
    rejected(() => db.query('SELECT zeroblob(1048576),zeroblob(1048576),zeroblob(1048576),zeroblob(1048576),zeroblob(1048576)'), 'aggregate result quota');
    rejected(() => db.exec("WITH RECURSIVE n(x) AS (VALUES(0) UNION ALL SELECT x+1 FROM n WHERE x<1100) INSERT INTO changes(label) SELECT 'limited-' || x FROM n RETURNING id"), 'RETURNING row quota');
    check(db.query("SELECT count(*) FROM changes WHERE label LIKE 'limited-%'").rows[0][0] === 0, 'statement quota failure rolls back');
    check(db.query('SELECT 7').rows[0][0] === 7, 'connection after quota failures');
  },
  sandbox() {
    for (const sql of [
      "ATTACH DATABASE ':memory:' AS escaped",
      'DETACH DATABASE escaped',
      'PRAGMA database_list',
      'PRAGMA writable_schema=ON',
      "SELECT load_extension('/tmp/no-such-synthetic-extension')",
      'BEGIN', 'COMMIT', 'SAVEPOINT escaped', 'RELEASE escaped',
      'SELECT 1; SELECT 2',
      "CREATE TABLE multi_statement_escape(value); INSERT INTO multi_statement_escape VALUES(1)"
    ]) rejected(() => db.exec(sql), sql);
    check(db.query("SELECT count(*) FROM sqlite_master WHERE name='multi_statement_escape'").rows[0][0] === 0, 'multi-statement atomic rejection');
    rejected(() => ircd.sqlite.open('ungranted'), 'ungranted database');
    for (const name of ['', '../rss', '/tmp/rss', 'rss.sqlite', 'rss/other', 'file:rss?mode=ro', 'rss\u0000', '\u00e9'])
      rejected(() => ircd.sqlite.open(name), 'unsafe database name');
    check(db.query('SELECT 6').rows[0][0] === 6, 'connection after denied SQL');
  },
  isolation() {
    const other = ircd.sqlite.open('other');
    other.exec('CREATE TABLE IF NOT EXISTS state (key TEXT PRIMARY KEY, value INTEGER)');
    other.exec("INSERT OR REPLACE INTO state VALUES('durable', 99)");
    check(other.query("SELECT value FROM state WHERE key='durable'").rows[0][0] === 99, 'other database');
    check(db.query("SELECT value FROM state WHERE key='durable'").rows[0][0] === 73, 'named database isolation');
    other.close();
    rejected(() => other.query('SELECT 1'), 'closed query');
    rejected(() => other.exec('SELECT 1'), 'closed exec');
    rejected(() => other.transaction(() => {}), 'closed transaction');
    other.close();
  },
  bench_read() { check(db.query("SELECT value FROM state WHERE key='durable'").rows[0][0] === 73, 'benchmark read'); },
  bench_write() { db.exec("UPDATE state SET value=value+1 WHERE key='benchmark'"); },
  bench_transaction() { db.transaction(() => db.exec("UPDATE state SET value=value+1 WHERE key='benchmark'")); },
  bench_initialize() { db.exec("INSERT OR REPLACE INTO state VALUES('benchmark', 0)"); },
  bench_check() { check(db.query("SELECT value FROM state WHERE key='benchmark'").rows[0][0] === 100, 'benchmark write count'); },
  bump() { check(++heap === 1, 'fresh heap first mutation'); },
  retained() {
    check(heap === 1, 'failed reload must retain old heap');
    check(db.query("SELECT count(*) FROM changes WHERE label='failed-reload-write'").rows[0][0] === 0, 'failed initialization write');
  },
  denied() { check(count('deny-commit') === 1, 'hook deny must not silently rollback committed SQL'); },
  restored() {
    check(heap === 0 && initDurable === 73, 'durable database and disposable heap');
    check(count('committed') === 1 && count('deny-commit') === 1, 'durable committed rows');
    check(count('sql-rollback') === 0 && count('js-rollback') === 0 && count('promise-rollback') === 0 && count('nested-rollback') === 0, 'rollbacks remain absent');
  }
};
ircd.on('preCommand', e => {
  if (e.validated) return;
  if (e.command === 'XSQLBENCH') {
    let outcome = 'ok';
    try { cases['bench_' + e.params[0]](); }
    catch (error) { outcome = 'FAIL:' + String(error.message); }
    // One response packet avoids delayed-ACK/Nagle timing artifacts caused by
    // the normal unknown-command numeric followed by a queued NOTICE.
    return {action:'pass', command:'PING', params:['sqlite-bench-' + e.params[0] + ':' + outcome]};
  }
  if (e.command !== 'XSQLITE') return;
  const name = e.params[0];
  if (name === 'deny') {
    db.exec("INSERT INTO changes(label) VALUES('deny-commit')");
    return {action:'deny'};
  }
  try {
    check(typeof cases[name] === 'function', 'unknown case');
    cases[name]();
    ircd.notice(e.userId, 'sqlite:' + name + ':ok');
  } catch (error) {
    ircd.notice(e.userId, 'sqlite:' + name + ':FAIL:' + String(error.message));
  }
});
"""

REVOKED = r"""
function denied() {
  try { ircd.sqlite.open('rss'); } catch (_) { return true; }
  return false;
}
if (!denied()) throw Error('ungranted initialization open');
ircd.on('preCommand', e => {
  if (!e.validated && e.command === 'XSQLITE')
    ircd.notice(e.userId, 'sqlite:revoked:' + (denied() ? 'ok' : 'FAIL:ungranted runtime open'));
});
"""

TIMEOUT = r"""
const db = ircd.sqlite.open('rss');
ircd.on('preCommand', e => {
  if (e.validated || e.command !== 'XSQLITE') return;
  db.exec("INSERT OR REPLACE INTO state VALUES('sql-timeout-entered', 1)");
  db.transaction(() => {
    db.exec("INSERT INTO changes(label) VALUES('timeout-rollback')");
    db.query('WITH RECURSIVE n(x) AS (VALUES(0) UNION ALL SELECT x+1 FROM n WHERE x<1000000000) SELECT sum(x) FROM n');
  });
});
"""


V8_TIMEOUT = r"""
const db = ircd.sqlite.open('rss');
ircd.on('preCommand', e => {
  if (e.validated || e.command !== 'XSQLITE') return;
  db.exec("INSERT OR REPLACE INTO state VALUES('v8-timeout-entered', 1)");
  db.transaction(() => {
    db.exec("INSERT INTO changes(label) VALUES('v8-timeout-rollback')");
    while (true) {}
  });
});
"""


class Harness:
    def __init__(self, path):
        self.path = path
        self.script = path / "hooks.js"
        self.config = path / "inspircd.conf"
        self.database_dir = path / "private-databases"
        self.database_dir.mkdir(mode=0o700)
        self.log = path / "server.log"
        self.server = self.client = self.log_handle = None
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            self.port = sock.getsockname()[1]
        self.base = f'''<server name="sqlite.test" description="Synthetic SQLite tests" id="001" network="SQLiteTest">
<admin name="Test" email="test@example.invalid">
<bind address="127.0.0.1" port="{self.port}" type="clients">
<connect name="test" allow="127.0.0.1" maxlocal="20" maxglobal="20" fakelag="yes" threshold="100000">
<security user="{pwd.getpwuid(os.getuid()).pw_name}" group="{grp.getgrgid(os.getgid()).gr_name}">
<module name="javascript">
<log method="stdout" type="m_javascript MODULE CONFIG" level="normal">
<javascriptsettings timeoutms="200" sqlitedirectory="{self.database_dir}">
'''

    def configure(self, source, grants="rss other", module=True, extra=()):
        self.script.write_text(source)
        base = self.base if module else self.base.replace('<module name="javascript">', "")
        content = base + f'<javascript file="{self.script}" databases="{grants}">\n'
        for index, (extra_source, extra_grants) in enumerate(extra):
            extra_path = self.path / f"extra-{index}.js"
            extra_path.write_text(extra_source)
            content += f'<javascript file="{extra_path}" databases="{extra_grants}">\n'
        self.config.write_text(content)

    def start(self, source, grants="rss other"):
        self.configure(source, grants)
        offset = self.log.stat().st_size if self.log.exists() else 0
        self.log_handle = self.log.open("a")
        self.server = subprocess.Popen(
            [str(RUN / "bin/inspircd"), "--nofork", "--config", str(self.config)],
            cwd=RUN,
            stdout=self.log_handle,
            stderr=subprocess.STDOUT,
        )
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            output = self.log.read_text()[offset:]
            if self.server.poll() is not None:
                raise AssertionError("Native server failed to start:\n" + output)
            if "JavaScript configuration loaded" in output:
                self.client = Client("127.0.0.1", self.port, "sqlcaller")
                return
            time.sleep(0.05)
        raise AssertionError("Native SQLite module did not initialize:\n" + output)

    def reload(self, source, grants="rss other", success=True, module=True, extra=()):
        self.configure(source, grants, module, extra)
        offset = self.log.stat().st_size
        self.server.send_signal(signal.SIGHUP)
        deadline = time.monotonic() + 15
        wanted = (
            "JavaScript configuration loaded"
            if module
            else "The m_javascript.so module was unloaded"
        )
        while time.monotonic() < deadline:
            output = self.log.read_text()[offset:]
            assert self.server.poll() is None, output
            failed = "Unable to read the configuration for m_javascript" in output
            if wanted in output or failed:
                assert (not failed) == success, output
                return output
            time.sleep(0.05)
        raise AssertionError("Rehash did not complete:\n" + output)

    def case(self, name):
        self.client.send("XSQLITE " + name)
        marker = ":sqlite:" + name + ":"
        lines = self.client.until(lambda line: marker in line, timeout=5)
        response = next(line.split(marker, 1)[1] for line in lines if marker in line)
        assert response == "ok", (name, response, lines)
        self.client.barrier("done-" + name)

    def benchmark(self, operation):
        marker = "sqlite-bench-" + operation + ":"
        began = time.perf_counter()
        self.client.send("XSQLBENCH " + operation)
        lines = self.client.until(lambda line: " PONG " in line and marker in line, timeout=5)
        elapsed_ms = (time.perf_counter() - began) * 1000
        assert lines[-1].endswith(marker + "ok"), lines
        return elapsed_ms

    def stop(self):
        if self.client is not None:
            self.client.close()
            self.client = None
        if self.server is not None:
            if self.server.poll() is None:
                self.server.terminate()
            try:
                self.server.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.server.kill()
                self.server.wait(timeout=5)
            self.server = None
        if self.log_handle is not None:
            self.log_handle.close()
            self.log_handle = None


def database_files(harness):
    files = []
    for path in harness.database_dir.rglob("*"):
        if path.is_file():
            with path.open("rb") as candidate:
                if candidate.read(16) == b"SQLite format 3\x00":
                    files.append(path)
    assert len(files) == 2, files
    for path in files:
        assert path.stat().st_mode & 0o077 == 0, ("database must be private", path)
    return files


def database_fds(harness):
    directory = pathlib.Path("/proc") / str(harness.server.pid) / "fd"
    if not directory.exists():
        return None
    opened = []
    for fd in directory.iterdir():
        with contextlib.suppress(FileNotFoundError):
            target = os.readlink(fd)
            if str(harness.database_dir) in target:
                opened.append(target)
    return opened


def check_durable_files(harness, entered=None):
    seen = {}
    for path in database_files(harness):
        with contextlib.closing(sqlite3.connect(path, timeout=0.5)) as db:
            value = db.execute("SELECT value FROM state WHERE key='durable'").fetchone()[0]
            seen[value] = path
            if value == 73:
                if entered is not None:
                    assert db.execute(
                        "SELECT value FROM state WHERE key=?", (entered,)
                    ).fetchone() == (1,), "Timeout callback did not execute"
                labels = {row[0] for row in db.execute("SELECT label FROM changes")}
                assert labels == {"committed", "after-rollback", "deny-commit"}, labels
                assert db.execute("PRAGMA integrity_check").fetchone() == ("ok",)
    assert set(seen) == {73, 99}, seen


def main():
    with tempfile.TemporaryDirectory(prefix="irc-javascript-sqlite-") as directory:
        harness = Harness(pathlib.Path(directory))
        try:
            harness.start(PROBE)
            harness.case("probe")
            print("PASS: compiled module exposes ircd.sqlite.open", flush=True)
            harness.reload(SUITE)
            for name in (
                "initialize",
                "binds",
                "commit",
                "rollback",
                "autoabort",
                "synchronous",
                "malformed",
                "limits",
                "sandbox",
                "isolation",
                "bump",
            ):
                harness.case(name)
                print("PASS: native SQLite " + name, flush=True)
            harness.client.send("XSQLITE deny")
            lines = harness.client.barrier("deny-finished")
            assert not any(" XSQLITE :Unknown command" in line for line in lines), lines
            harness.case("denied")
            print(
                "PASS: committed SQL survives hook deny independently of queued IRC effects",
                flush=True,
            )

            # Candidate initialization cannot write, and a failed candidate must
            # leave the working isolate and all durable bytes untouched.
            harness.reload(
                "const db=ircd.sqlite.open('rss'); db.exec(\"INSERT INTO changes(label) VALUES('failed-reload-write')\");",
                success=False,
            )
            harness.case("retained")
            harness.reload(SUITE)
            harness.case("restored")
            harness.case("bump")
            harness.stop()
            harness.start(SUITE)
            harness.case("restored")
            print(
                "PASS: failed reload preserves active heap; successful reload/restart reset heap and preserve SQLite",
                flush=True,
            )

            # Simultaneously loaded scripts do not share grants. The secondary
            # script may read its own named DB but cannot open the primary DB.
            scoped = REVOKED.replace("'XSQLITE'", "'XSCOPED'").replace(
                "sqlite:revoked:", "sqlite:scoped:"
            )
            scoped += "\nif (ircd.sqlite.open('other').query(\"SELECT value FROM state WHERE key='durable'\").rows[0][0] !== 99) throw Error('granted other database');"
            harness.reload(SUITE, grants="rss", extra=[(scoped, "other")])
            harness.client.send("XSCOPED")
            harness.client.until(lambda line: line.endswith(":sqlite:scoped:ok"))
            harness.case("restored")
            print("PASS: concurrent scripts have separate database grants", flush=True)

            # Grants are per candidate configuration, not inherited by new
            # scripts from a previously authorized isolate.
            harness.reload(REVOKED, grants="")
            harness.case("revoked")
            harness.reload(SUITE)
            harness.case("restored")
            harness.reload(REVOKED, grants="../escape", success=False)
            harness.case("restored")
            print(
                "PASS: grant revocation, ungranted initialization/runtime opens, and invalid configured names",
                flush=True,
            )

            # A long-running SQLite VM must yield to the native callback budget.
            # A timeout may poison the isolate, so inspect/recover through rehash,
            # rather than relying on another callback in the timed-out isolate.
            for source, reason, entered in (
                (TIMEOUT, "recursive SQLite VM", "sql-timeout-entered"),
                (V8_TIMEOUT, "V8 execution", "v8-timeout-entered"),
            ):
                harness.reload(source)
                began = time.monotonic()
                harness.client.send("XSQLITE timeout")
                time.sleep(0.6)
                assert harness.server.poll() is None, reason + " timeout killed the daemon"
                harness.reload(SUITE)
                elapsed = time.monotonic() - began
                assert elapsed < 3, (reason + " failed to honor callback budget", elapsed)
                harness.case("restored")
                check_durable_files(harness, entered=entered)
                print(
                    "PASS: "
                    + reason
                    + " timeout returns control, rolls back, and permits reload recovery",
                    flush=True,
                )

            harness.case("bench_initialize")
            for operation in ("read", "write", "transaction"):
                elapsed_ms = []
                for _ in range(50):
                    elapsed_ms.append(harness.benchmark(operation))
                ordered = sorted(elapsed_ms)
                print(
                    f"TIMING: synthetic loopback {operation}, 50 callback+IRC round trips: "
                    f"p50={statistics.median(ordered):.3f} ms p95={ordered[47]:.3f} ms max={ordered[-1]:.3f} ms",
                    flush=True,
                )
            harness.case("bench_check")

            opened = database_fds(harness)
            assert opened is None or opened, "No actual SQLite file handles while loaded"
            harness.reload("", module=False)
            lines = harness.client.barrier("after-unload")
            harness.client.send("XSQLITE ignored")
            lines += harness.client.barrier("module-released")
            assert any(" XSQLITE :Unknown command" in line for line in lines), lines
            remaining = database_fds(harness)
            assert remaining is None or not remaining, (
                "SQLite resources survived module unload",
                remaining,
            )
            check_durable_files(harness)
            harness.reload(SUITE)
            harness.case("restored")
            print(
                "PASS: close rejects stale handles, module unload releases native resources, reload preserves disk state",
                flush=True,
            )
        except Exception:
            print(harness.log.read_text() if harness.log.exists() else "No native server log")
            raise
        finally:
            harness.stop()


if __name__ == "__main__":
    main()
