#!/usr/bin/env python3
"""Opt-in public feed -> native fetch/parser -> actual RSS script -> local IRC.

RSS_PUBLIC_FEED_URL=https://news.ycombinator.com/rss \
INSPIRCD_NATIVE=/path/to/install python3 tests/rss_fetch_native.py

This starts only a synthetic loopback daemon; no production channel is touched.
The feed must be public HTTP(S), nonempty and readable under the unchanged native
transport policy. External data changes can affect this optional integration test.

For deterministic fixtures, compile the isolated daemon module with
JS_ASYNC_HTTP_TEST_LOOPBACK, then use RSS_TEST_LOOPBACK=1. That build-only seam
permits literal 127.0.0.1 HTTP. The harness appends a test-only host-call adapter
mapping its default-port subscription to the fixture's ephemeral port; production
RSS validation/algorithm is unchanged. Never install the test build in production.
"""

import grp
import json
import os
import pathlib
import pwd
import socket
import sqlite3
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

from precommand_client import Client

# Test-only observation/clock advancement. The production script remains intact.
# Clearing its baseline once simulates new entries without a local fetch exception.
PROBE = r"""
let rssTestPhase = 'baseline';
let rssTestWaiting = false;
ircd.on('postCommand', e => {
  if (e.command !== 'PING') return;
  if (e.params[0] !== 'rss-test-new' && e.params[0] !== 'rss-test-repeat') return;
  const sub = [...rssSubscriptions.values()][0];
  if (!sub || !sub.seeded || sub.job !== null || sub.pending.length) throw Error('test not ready');
  if (!rssTestFixture && e.params[0] === 'rss-test-new') {
    sub.seen.clear();
    const testDb = ircd.sqlite.open('rss');
    testDb.exec('DELETE FROM rss_seen WHERE subscription = (SELECT id FROM rss_subscriptions WHERE key = ?)', [sub.channelKey + '\n' + sub.feed]);
    testDb.close();
  }
  sub.nextDue = 0;
  rssTestPhase = e.params[0] === 'rss-test-new' ? 'new' : 'repeat';
  rssTestWaiting = false;
});
ircd.on('backgroundTimer', () => {
  const sub = [...rssSubscriptions.values()][0];
  if (!sub || rssTestPhase === 'idle') return;
  if (sub.failures) {
    ircd.notice('#rss-fetch-test', 'rss-test-fetch-failed');
    rssTestPhase = 'idle';
    return;
  }
  if (rssTestPhase === 'baseline' && sub.seeded && sub.job === null && sub.nextDue > 0) {
    ircd.notice('#rss-fetch-test', (sub.seen.size || (rssTestFixture && rssStorage.known(sub.channel, sub.feed, ['old']).size)) ? 'rss-test-baseline' : 'rss-test-empty-feed');
    rssTestPhase = 'idle';
    return;
  }
  if (sub.job !== null) rssTestWaiting = true;
  if (rssTestWaiting && sub.job === null && sub.pending.length === 0) {
    ircd.notice('#rss-fetch-test', 'rss-test-' + rssTestPhase + '-done');
    rssTestPhase = 'idle';
  }
});
"""


def exercise(url, fixture_port=None):
    parts = urlsplit(url)
    assert parts.scheme in ("http", "https") and parts.hostname, "Expected public HTTP(S) URL"
    assert not parts.username and not parts.password and not parts.fragment, (
        "No credentials/fragments"
    )
    assert not any(c.isspace() or ord(c) < 32 for c in url), "Invalid URL characters"
    assert len(url.encode()) <= 350, "Test URL must fit an ordinary IRC command"
    run = pathlib.Path(os.environ["INSPIRCD_NATIVE"]).resolve()
    root = pathlib.Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="irc-rss-fetch-") as directory:
        path = pathlib.Path(directory)
        database_root = path / "databases"
        database_root.mkdir(mode=0o700)
        script = path / "rss.js"
        adapter = "const rssTestFixture = " + ("true" if fixture_port else "false") + ";\n"
        if fixture_port:
            adapter += (
                "const rssTestFetch = ircd.fetch;\n"
                "ircd.fetch = url => rssTestFetch(url === 'http://127.0.0.1/rss' ? "
                + json.dumps(f"http://127.0.0.1:{fixture_port}/rss")
                + " : url);\n"
            )
        script.write_text((root / "examples/rss.js").read_text() + "\n" + adapter + PROBE)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        config = path / "inspircd.conf"
        config.write_text(f'''<server name="rss-fetch.test" description="Isolated RSS fetch test" id="001" network="RSSTest">
<admin name="Test" email="test@example.invalid">
<pid file="{path / "daemon.pid"}">
<path moduledir="{run / "modules"}">
<bind address="127.0.0.1" port="{port}" type="clients">
<connect name="test" allow="127.0.0.1" maxlocal="20" maxglobal="20" fakelag="yes" threshold="100000">
<security user="{pwd.getpwuid(os.getuid()).pw_name}" group="{grp.getgrgid(os.getgid()).gr_name}">
<class name="RSS" commands="RSS">
<type name="RSSOperator" classes="RSS">
<oper name="rss-test" password="synthetic-rss-test-only" host="*@127.0.0.1" type="RSSOperator">
<module name="javascript">
<log method="stdout" type="m_javascript MODULE CONFIG" level="normal">
<javascriptsettings timeoutms="25" sqlitedirectory="{database_root}">
<javascript file="{script}" authority="admin" databases="rss">
''')
        logfile = path / "daemon.log"
        client = None
        with logfile.open("w") as log:
            server = subprocess.Popen(
                [str(run / "bin/inspircd"), "--nofork", "--config", str(config)],
                cwd=run,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            try:
                time.sleep(0.3)
                assert server.poll() is None, logfile.read_text()
                client = Client("127.0.0.1", port, "rssfetcher")
                client.send("JOIN #rss-fetch-test")
                client.barrier("rss-fetch-joined")
                client.send("OPER rss-test synthetic-rss-test-only")
                client.until(lambda line: " 381 rssfetcher " in line)
                client.send("RSS #rss-fetch-test " + url)
                subscribed = client.until(lambda line: "RSS subscribed." in line)
                assert not any(" PRIVMSG #rss-fetch-test :[RSS] " in line for line in subscribed)

                def phase(marker):
                    lines = client.until(
                        lambda line: (
                            line.endswith(":" + marker)
                            or line.endswith(":rss-test-fetch-failed")
                            or line.endswith(":rss-test-empty-feed")
                        ),
                        timeout=45,
                    )
                    assert not any(line.endswith(":rss-test-fetch-failed") for line in lines), (
                        "Native RSS fetch failed; public mode requires DNS/egress/TLS/feed availability; "
                        "fixture mode requires the explicit JS_ASYNC_HTTP_TEST_LOOPBACK test build."
                    )
                    assert not any(line.endswith(":rss-test-empty-feed") for line in lines), (
                        "The public feed supplied no usable entries; choose a populated stable feed."
                    )
                    assert lines[-1].endswith(":" + marker), lines
                    return [line for line in lines if " PRIVMSG #rss-fetch-test :[RSS] " in line]

                assert phase("rss-test-baseline") == [], "Baseline must not flood the channel"
                client.send("PING :rss-test-new")
                first = phase("rss-test-new-done")
                assert first, "Successful native fetch must yield actual parsed RSS channel posts"
                assert all(line.startswith(":rss-fetch.test PRIVMSG ") for line in first), first
                assert len(first) == len(set(first)), "Duplicate posts in first fetch"
                client.send("PING :rss-test-repeat")
                second = phase("rss-test-repeat-done")
                assert not set(first).intersection(second), (
                    "Repeated feed entries were posted twice"
                )
                if fixture_port:
                    assert len(first) == 8, first
                    assert all("New fixture entry " in line for line in first), first
                    assert second == [], second
                    # A real SQLite exclusive lock must leave both durable and live
                    # subscriptions intact when an unsubscribe transaction fails.
                    lock = sqlite3.connect(database_root / "rss.sqlite", timeout=0)
                    lock.execute("BEGIN EXCLUSIVE")
                    client.send("RSS #rss-fetch-test off")
                    denied = client.until(lambda line: "RSS storage" in line)
                    assert not any("subscriptions removed" in line for line in denied), denied
                    assert lock.execute("SELECT COUNT(*) FROM rss_subscriptions").fetchone()[0] == 1
                    lock.rollback()
                    lock.close()

                    # Restart the actual daemon, preserving only the SQLite directory.
                    client.close()
                    client = None
                    server.terminate()
                    server.wait(timeout=5)
                    server = subprocess.Popen(
                        [str(run / "bin/inspircd"), "--nofork", "--config", str(config)],
                        cwd=run,
                        stdout=log,
                        stderr=subprocess.STDOUT,
                    )
                    time.sleep(0.3)
                    assert server.poll() is None, logfile.read_text()
                    client = Client("127.0.0.1", port, "rssfetcher")
                    client.send("JOIN #rss-fetch-test")
                    client.barrier("rss-restarted-joined")
                    client.send("OPER rss-test synthetic-rss-test-only")
                    client.until(lambda line: " 381 rssfetcher " in line)
                    assert phase("rss-test-baseline") == [], "Restored baseline must not repost"
                    client.send("PING :rss-test-repeat")
                    assert phase("rss-test-repeat-done") == [], "Restart must retain dedup IDs"
                client.send("RSS #rss-fetch-test off")
                client.until(lambda line: "RSS subscriptions removed" in line)
                with sqlite3.connect(database_root / "rss.sqlite") as check:
                    assert (
                        check.execute("SELECT COUNT(*) FROM rss_subscriptions").fetchone()[0] == 0
                    )
                    assert check.execute("SELECT COUNT(*) FROM rss_seen").fetchone()[0] == 0
                print(
                    f"PASS: {'loopback fixture' if fixture_port else 'public feed ' + parts.hostname}, native HTTP + JavaScript XML parser, operator /RSS, "
                    f"baseline, {len(first)} real channel PRIVMSG posts, busy rollback and restart deduplication"
                )
            except Exception:
                print(logfile.read_text())
                raise
            finally:
                if client:
                    client.close()
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)


def main():
    if os.environ.get("RSS_TEST_LOOPBACK") == "1":

        class Fixture(BaseHTTPRequestHandler):
            requests = 0

            def do_GET(self):
                assert self.path == "/rss"
                type(self).requests += 1
                new = (
                    ""
                    if self.requests == 1
                    else (
                        "".join(
                            f"<item><guid>new{i}</guid><title>New fixture entry {i}</title>"
                            f"<link>https://example.org/new{i}</link></item>"
                            for i in range(8)
                        )
                    )
                )
                body = (
                    '<?xml version="1.0"?><rss version="2.0"><channel>'
                    "<title>Isolated test feed</title>"
                    + new
                    + "<item><guid>old</guid><title>Old fixture entry</title>"
                    "<link>https://example.org/old</link></item></channel></rss>"
                ).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/rss+xml")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        http = ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
        thread = threading.Thread(target=http.serve_forever, daemon=True)
        thread.start()
        try:
            exercise("http://127.0.0.1/rss", http.server_port)
            assert Fixture.requests == 5, Fixture.requests
        finally:
            http.shutdown()
            http.server_close()
            thread.join(timeout=5)
        return
    url = os.environ.get("RSS_PUBLIC_FEED_URL")
    if url:
        exercise(url)
    else:
        print("SKIP: set RSS_PUBLIC_FEED_URL, or RSS_TEST_LOOPBACK=1 with a test-only module build")


if __name__ == "__main__":
    main()
