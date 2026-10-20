#!/usr/bin/env python3
"""Exercise URL-scoped host headers through real V8 in a disposable IRC daemon.

Requires INSPIRCD_NATIVE=/path/to/install and INSPIRCD_HTTP_TEST_LOOPBACK=1,
with the module built using JS_ASYNC_HTTP_TEST_LOOPBACK. No existing server,
configuration, credentials, or public HTTP endpoint is used.
"""

import grp
import html
import json
import os
import pwd
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path

from precommand_client import Client
from test_js_webhooks import Receiver


def source(base, command, owner):
    return (
        f"const base={json.dumps(base)};\n"
        + r"""
ircd.registerCommand(COMMAND, 2, 2, false, e => {
  ircd.fetch(base + e.params[0], {
    method: 'POST', body: JSON.stringify({source: OWNER, marker: e.params[1]})
  });
  ircd.notice(e.userId, 'staged:' + e.params[1]);
});
""".replace("COMMAND", json.dumps(command)).replace("OWNER", json.dumps(owner))
    )


OVERRIDE = r"""
ircd.registerCommand('FETCHOVERRIDE', 0, 0, false, e => {
  let rejected = false;
  try {
    ircd.fetch(base + '/override-must-not-send', {
      method: 'POST', body: 'must-not-send', headers: {'xc-token': 'synthetic-override'}
    });
  } catch (error) {
    rejected = String(error).includes('Unknown fetch option');
  }
  ircd.notice(e.userId, 'option:' + (rejected ? 'rejected' : 'ACCEPTED'));
});
"""

WEBHOOK_ORDER = r"""
ircd.registerCommand('WEBHOOKCONTROL', 0, 0, false, () => {
  ircd.postWebhook('unload', '{"marker":"webhook-control"}');
});
ircd.registerCommand('WEBHOOKUNLOAD', 0, 0, false, () => {
  ircd.unloadModule('m_javascript.so');
  ircd.postWebhook('unload', '{"marker":"must-not-send-after-unload"}');
});
"""


def main():
    if os.environ.get("INSPIRCD_HTTP_TEST_LOOPBACK") != "1":
        raise SystemExit("This isolated test requires INSPIRCD_HTTP_TEST_LOOPBACK=1")
    run = Path(os.environ["INSPIRCD_NATIVE"]).resolve()
    with tempfile.TemporaryDirectory(prefix="irc-native-fetch-headers-") as directory:
        path = Path(directory)
        receiver = Receiver()
        first, second, secret = path / "first.js", path / "second.js", path / "synthetic-secret"
        first.write_text(source(receiver.url, "FETCHONE", "first") + OVERRIDE)
        second.write_text(source(receiver.url, "FETCHTWO", "second"))
        secret.write_text("synthetic-first-token")
        secret.chmod(0o600)
        target = "/target?a=1&b=2"
        url = receiver.url + target
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        base = f'''<server name="fetch.test" description="Disposable fetch header tests" id="001" network="FetchTest">
<admin name="Test" email="test@example.invalid">
<bind address="127.0.0.1" port="{port}" type="clients">
<connect name="test" allow="127.0.0.1" maxlocal="20" maxglobal="20" fakelag="yes" threshold="100000">
<security user="{pwd.getpwuid(os.getuid()).pw_name}" group="{grp.getgrgid(os.getgid()).gr_name}">
<module name="javascript">
<log method="stdout" type="m_javascript MODULE CONFIG" level="normal">
<javascriptsettings timeoutms="200">
'''
        config, logfile = path / "inspircd.conf", path / "server.log"

        def rule(script=first, name="xc-token"):
            return (
                f'<javascriptfetchheader script="{script}" url="{html.escape(url, quote=True)}" '
                f'name="{name}" valuefile="{secret}">\n'
            )

        def configure(rules, first_authority="admin"):
            config.write_text(
                base
                + f'<javascript file="{first}" authority="{first_authority}">\n'
                + f'<javascript file="{second}" authority="admin">\n'
                + rules
            )

        configure(rule())
        with logfile.open("w") as log:
            server = subprocess.Popen(
                [str(run / "bin/inspircd"), "--nofork", "--config", str(config)],
                cwd=run,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            caller = None
            expected_markers = []
            try:
                time.sleep(0.4)
                assert server.poll() is None, logfile.read_text()
                caller = Client("127.0.0.1", port, "fetchcaller")

                def reload(rules, first_authority="admin", error=None):
                    configure(rules, first_authority)
                    offset = logfile.stat().st_size
                    server.send_signal(signal.SIGHUP)
                    deadline = time.monotonic() + 15
                    while time.monotonic() < deadline:
                        output = logfile.read_text()[offset:]
                        if "Unable to read the configuration for m_javascript" in output:
                            assert error is not None and error in output, output
                            return
                        if "JavaScript configuration loaded" in output:
                            assert error is None, "Invalid candidate unexpectedly loaded: " + output
                            return
                        assert server.poll() is None, output
                        time.sleep(0.05)
                    raise AssertionError("Rehash did not complete: " + output)

                def post(marker, token, route=target, command="FETCHONE"):
                    before = len(receiver.posts)
                    caller.send(f"{command} {route} {marker}")
                    caller.until(lambda line: line.endswith(":staged:" + marker))
                    posts = receiver.wait(before + 1, timeout=10)
                    assert len(posts) == before + 1, posts
                    received, body, headers = posts[-1]
                    headers = {key.lower(): value for key, value in headers.items()}
                    assert received == route, posts[-1]
                    assert headers.get("xc-token") == token, (marker, headers)
                    assert json.loads(body) == {
                        "source": "second" if command == "FETCHTWO" else "first",
                        "marker": marker,
                    }, body
                    expected_markers.append(marker)

                # Positive control must receive a credential, proving this is not
                # a vacuous no-I/O pass with the fixture exception disabled.
                post("positive", "synthetic-first-token")
                post("other-script", None, command="FETCHTWO")
                for marker, route in (
                    ("path-prefix", "/target/child?a=1&b=2"),
                    ("query-value", "/target?a=1&b=3"),
                    ("query-order", "/target?b=2&a=1"),
                    ("query-extra", "/target?a=1&b=2&extra=1"),
                    ("query-absent", "/target"),
                ):
                    post(marker, None, route=route)
                caller.send("FETCHOVERRIDE")
                lines = caller.until(lambda line: ":option:" in line)
                assert any(line.endswith(":option:rejected") for line in lines), lines
                post("after-option-rejection", "synthetic-first-token")
                print(
                    "PASS: real V8 URL matching, independent admin source, rejected JS headers option"
                )

                # Changing the file alone must not mutate an existing generation.
                secret.write_text("synthetic-rotated-token")
                post("immutable-before-rehash", "synthetic-first-token")
                # A duplicate normalized header and a rule targeting a user script
                # both fail the candidate and preserve the previous admin context.
                reload(
                    rule() + rule(name="XC-TOKEN"), error="Duplicate or excessive webhook headers"
                )
                post("duplicate-kept-old", "synthetic-first-token")
                reload(
                    rule(),
                    first_authority="user",
                    error="Fetch header requires a configured admin script",
                )
                post("authority-kept-old", "synthetic-first-token")
                reload(rule())
                post("rotated-after-rehash", "synthetic-rotated-token")
                reload("")
                post("removed-rule", None)
                post("other-script-after-removal", None, command="FETCHTWO")
                assert [
                    json.loads(item[1])["marker"] for item in receiver.posts
                ] == expected_markers
                output = logfile.read_text()
                for marker in (
                    "synthetic-first-token",
                    "synthetic-rotated-token",
                    "synthetic-override",
                ):
                    assert marker not in output, "Header value leaked into daemon log"
                print(
                    "PASS: immutable secret snapshot, duplicate/authority rollback, rotation and rule removal"
                )

                # Named webhooks must use the same ordered effect queue as fetch:
                # a prior self-unload invalidates later staged network effects.
                first.write_text(WEBHOOK_ORDER)
                reload(
                    f'<javascriptwebhook name="unload" url="{receiver.url}/webhook-unload" '
                    'loopbackhttp="yes">\n'
                    f'<javascriptwebhookheader endpoint="unload" name="xc-token" valuefile="{secret}">\n'
                )
                before = len(receiver.posts)
                caller.send("WEBHOOKCONTROL")
                caller.barrier("webhook-control-committed")
                posts = receiver.wait(before + 1, timeout=10)
                route, body, headers = posts[-1]
                headers = {key.lower(): value for key, value in headers.items()}
                assert len(posts) == before + 1 and route == "/webhook-unload", posts
                assert json.loads(body) == {"marker": "webhook-control"}, body
                assert headers.get("xc-token") == "synthetic-rotated-token", headers
                assert headers.get("content-type") == "application/json", headers
                caller.send("WEBHOOKUNLOAD")
                caller.barrier("webhook-self-unloaded")
                caller.send("WEBHOOKCONTROL")
                lines = caller.barrier("webhook-command-removed")
                assert any(" 421 " in line and "WEBHOOKCONTROL" in line for line in lines), lines
                with receiver.condition:
                    assert not receiver.condition.wait_for(
                        lambda: len(receiver.posts) > before + 1, timeout=2
                    ), receiver.posts
                print("PASS: named webhook positive control and self-unload suppresses later POST")
            except BaseException:
                print(logfile.read_text())
                raise
            finally:
                if caller is not None:
                    caller.close()
                if server.poll() is None:
                    server.terminate()
                    server.wait(timeout=10)
                receiver.close()


if __name__ == "__main__":
    main()
