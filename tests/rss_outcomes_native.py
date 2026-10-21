#!/usr/bin/env python3
"""Isolated native RSS outcomes, disabled handlers, persistence and log redaction."""

import grp
import os
import pathlib
import pwd
import signal
import socket
import subprocess
import tempfile
import time

from precommand_client import Client


def main():
    root = pathlib.Path(__file__).resolve().parents[1]
    run = pathlib.Path(os.environ["INSPIRCD_NATIVE"]).resolve()
    source = (root / "examples/rss.js").read_text()
    # Never contact a feed host in this outcome test.
    source += "\nircd.fetch = () => { throw Error('synthetic-no-network'); };\n"
    with tempfile.TemporaryDirectory(prefix="rss-outcomes-") as directory:
        path = pathlib.Path(directory)
        (path / "db").mkdir(mode=0o700)
        script = path / "rss.js"
        script.write_text(source)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        config = path / "inspircd.conf"
        config.write_text(f'''<server name="rss.test" description="Isolated test" id="001" network="Test">
<admin name="Test" email="test@example.invalid">
<pid file="{path / "pid"}">
<path moduledir="{run / "modules"}">
<bind address="127.0.0.1" port="{port}" type="clients">
<connect allow="127.0.0.1" fakelag="yes" threshold="100000">
<security user="{pwd.getpwuid(os.getuid()).pw_name}" group="{grp.getgrgid(os.getgid()).gr_name}">
<module name="javascript">
<log method="stdout" type="m_javascript MODULE CONFIG" level="normal">
<javascriptsettings timeoutms="200" sqlitedirectory="{path / "db"}">
<javascript file="{script}" authority="admin" databases="rss">
''')
        logpath = path / "server.log"
        with logpath.open("w") as log:
            server = subprocess.Popen(
                [str(run / "bin/inspircd"), "--nofork", "--config", str(config)],
                cwd=run,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            client = None
            try:
                time.sleep(0.3)
                assert server.poll() is None, logpath.read_text()
                client = Client("127.0.0.1", port, "rssuser")
                client.send("JOIN #rss-test")
                client.barrier("joined")

                def command(args, expected):
                    client.send("RSS" + (" " + args if args else ""))
                    return client.until(lambda line: expected in line)

                def reload(text):
                    script.write_text(text)
                    offset = logpath.stat().st_size
                    server.send_signal(signal.SIGHUP)
                    deadline = time.monotonic() + 10
                    while time.monotonic() < deadline:
                        if "JavaScript configuration loaded" in logpath.read_text()[offset:]:
                            return
                        time.sleep(0.05)
                    raise AssertionError(logpath.read_text()[offset:])

                for args in ("", "list", "#rss-test list extra"):
                    command(args, "RSS usage:")
                command("#missing list", "RSS requires an existing channel")
                command("#rss-test list", "RSS list: no subscriptions")
                command("#rss-test not-a-url", "RSS requires a public")
                feed = "https://example.invalid/private-feed-sentinel?token=private-query-sentinel"
                command("#rss-test " + feed, "RSS subscribed.")
                lines = command("#rss-test list", "RSS list complete: 1")
                assert any(feed in line for line in lines), lines
                command("#rss-test " + feed, "RSS subscription already exists.")
                reload(source)
                command("#rss-test list", "RSS list complete: 1")
                for index in range(31):
                    command(
                        "#rss-test https://example.invalid/feed-" + str(index), "RSS subscribed."
                    )
                for page in range(1, 5):
                    args = "list" if page == 1 else "list:" + str(page)
                    lines = command("#rss-test " + args, "page " + str(page) + "/4")
                    assert sum("RSS feed:" in line for line in lines) == (10 if page < 4 else 2)
                command("#rss-test list:5", "RSS list page out of range")
                command("#rss-test off", "RSS subscriptions removed")
                command("#rss-test list", "RSS list: no subscriptions")
                reload(
                    source.replace(
                        "rssStorage.add(channel, feed);", "throw Error('private-storage-sentinel');"
                    )
                )
                command("#rss-test " + feed, "RSS storage unavailable; subscription not added.")
                command("#rss-test list", "RSS list: no subscriptions")
                reload(
                    "ircd.on('postCommand', e => { if (e.command === 'RSS') throw Error('private-exception-sentinel'); });"
                )
                command("#rss-test list", "RSS unavailable:")
                command("#rss-test list", "RSS unavailable:")  # Already unhealthy.
                reload(
                    "ircd.on('preCommand', e => { if (e.command === 'RSS') throw Error('private-pre-sentinel'); });"
                )
                command("#rss-test list", "RSS unavailable:")
                command("#rss-test list", "RSS unavailable:")
                reload(
                    "ircd.on('postCommand', e => e.command === 'RSS' ? {rssOutcome:'success'} : undefined);"
                )
                command("#rss-test list", "RSS unavailable:")
                reload("// No RSS handler")
                command("#rss-test list", "RSS unavailable:")
                output = logpath.read_text()
                assert "RSS command outcome=success" in output
                assert "RSS command outcome=failure" in output
                for secret in (
                    "private-feed-sentinel",
                    "private-query-sentinel",
                    "private-storage-sentinel",
                    "private-exception-sentinel",
                    "private-pre-sentinel",
                ):
                    assert secret not in output, secret
                print(
                    "PASS: RSS usage/list/add/duplicate/off/storage failure, unhealthy/missing handlers, persistence, sanitized outcome logs"
                )
            finally:
                if client:
                    client.close()
                if server.poll() is None:
                    server.terminate()
                    server.wait(timeout=10)


if __name__ == "__main__":
    main()
