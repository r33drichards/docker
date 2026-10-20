#!/usr/bin/env python3
"""Exercise deferred messaging APIs against the synthetic local IRC daemon."""

from precommand_client import Client


def _messages(lines, command, target, suffix):
    return [
        line for line in lines if f" {command} {target} :" in line and line.endswith(":" + suffix)
    ]


def exercise_messaging(caller, other, reload):
    """Test local socket delivery, authority, timers and deferred-effect lifetimes."""
    reload("")
    channel = "#rss-messaging"
    caller.send("JOIN " + channel)
    caller.barrier("rss-caller-joined")
    other.send("JOIN " + channel)
    other.barrier("rss-other-joined")
    caller.barrier("rss-members-ready")
    reload(
        r"""
let otherId;
ircd.on('postCommand', e => {
  if (e.command !== 'PING') return;
  if (e.params[0] === 'rss-remember-other') otherId = e.userId;
  if (e.params[0] === 'rss-send-all') {
    ircd.notice(otherId, 'rss-user-notice');
    ircd.privmsg(otherId, 'rss-user-privmsg');
    ircd.notice('#rss-messaging', 'rss-channel-notice');
    ircd.privmsg('#rss-messaging', 'rss-channel-privmsg');
  }
  if (e.params[0] === 'rss-missing-targets') {
    ircd.notice('999MISSING', 'rss-missing-user-notice');
    ircd.privmsg('999MISSING', 'rss-missing-user-privmsg');
    ircd.notice('#rss-missing', 'rss-missing-channel-notice');
    ircd.privmsg('#rss-missing', 'rss-missing-channel-privmsg');
    ircd.notice(e.userId, 'rss-missing-targets-done');
  }
});
""",
        "admin",
    )
    other.barrier("rss-remember-other")
    caller.send("PING :rss-send-all")
    received = other.until(lambda line: line.endswith(":rss-channel-privmsg"))
    expected = [
        ("NOTICE", other.nick, "rss-user-notice"),
        ("PRIVMSG", other.nick, "rss-user-privmsg"),
        ("NOTICE", channel, "rss-channel-notice"),
        ("PRIVMSG", channel, "rss-channel-privmsg"),
    ]
    for command, target, suffix in expected:
        matches = _messages(received, command, target, suffix)
        assert len(matches) == 1, (command, target, received)
        assert matches[0].startswith(":js.test "), matches
    received += other.barrier("rss-check-no-duplicates")
    for command, target, suffix in expected:
        assert len(_messages(received, command, target, suffix)) == 1, received
    own = caller.until(lambda line: line.endswith(":rss-channel-privmsg"))
    assert len(_messages(own, "NOTICE", channel, "rss-channel-notice")) == 1, own
    assert len(_messages(own, "PRIVMSG", channel, "rss-channel-privmsg")) == 1, own
    assert not any("rss-user-" in line for line in own), own
    caller.send("PING :rss-missing-targets")
    caller.until(lambda line: line.endswith(":rss-missing-targets-done"))
    missing = other.barrier("rss-absent-targets-drained")
    assert not any("rss-missing-" in line and " PONG " not in line for line in missing), missing

    # Actorless admin timers can emit both message types, once per armed callback.
    reload(
        r"""
let armed = false;
ircd.on('postCommand', e => {
  if (e.command === 'PING' && e.params[0] === 'rss-arm-timer') armed = true;
});
ircd.on('backgroundTimer', () => {
  if (!armed) return;
  armed = false;
  ircd.notice('#rss-messaging', 'rss-timer-notice');
  ircd.privmsg('#rss-messaging', 'rss-timer-privmsg');
});
""",
        "admin",
    )
    caller.send("PING :rss-arm-timer")
    for client in (caller, other):
        delivered = client.until(lambda line: line.endswith(":rss-timer-privmsg"), timeout=15)
        delivered += client.barrier("rss-timer-drained")
        for command, suffix in (("NOTICE", "rss-timer-notice"), ("PRIVMSG", "rss-timer-privmsg")):
            assert len(_messages(delivered, command, channel, suffix)) == 1, delivered

    # Native /RSS access checks reject nonoperators before successful command hooks.
    caller.send("RSS #rss-messaging https://example.org/feed")
    result = caller.barrier("rss-operator-check")
    assert any(f" 481 {caller.nick} " in line for line in result), result

    # Both APIs preserve user authority: own UUID works, other UUID/channel fails.
    reload(
        r"""
let otherId;
ircd.on('postCommand', e => {
  if (e.command !== 'PING') return;
  if (e.params[0] === 'rss-remember-other') otherId = e.userId;
  if (e.params[0] !== 'rss-user-authority') return;
  let rejected = 0;
  for (const method of ['notice', 'privmsg']) {
    for (const target of [otherId, '#rss-messaging']) {
      try { ircd[method](target, 'rss-forbidden'); }
      catch (_) { ++rejected; }
    }
  }
  if (rejected !== 4) throw Error('authority was not enforced');
  ircd.notice(e.userId, 'rss-self-notice');
  ircd.privmsg(e.userId, 'rss-self-privmsg');
});
"""
    )
    other.barrier("rss-remember-other")
    caller.send("PING :rss-user-authority")
    own = caller.until(lambda line: line.endswith(":rss-self-privmsg"))
    assert len(_messages(own, "NOTICE", caller.nick, "rss-self-notice")) == 1, own
    assert len(_messages(own, "PRIVMSG", caller.nick, "rss-self-privmsg")) == 1, own
    assert not any("rss-forbidden" in line for line in own), own
    assert not any("rss-forbidden" in line for line in other.barrier("rss-authority-drained"))

    # Strict text conversion rejects protocol injection and malformed UTF-16.
    reload(
        r"""
ircd.on('postCommand', e => {
  if (e.command !== 'PING' || e.params[0] !== 'rss-invalid-wire') return;
  let rejected = 0;
  for (const method of ['notice', 'privmsg']) {
    for (const text of ['line\r\ninjection', 'nul\x00injection', '\ud800', 'x'.repeat(401)]) {
      try { ircd[method]('#rss-messaging', text); }
      catch (_) { ++rejected; }
    }
  }
  if (rejected !== 8) throw Error('invalid wire data accepted');
  ircd.notice(e.userId, 'rss-invalid-wire-rejected');
});
""",
        "admin",
    )
    caller.send("PING :rss-invalid-wire")
    caller.until(lambda line: line.endswith(":rss-invalid-wire-rejected"))
    observed = other.barrier("rss-invalid-wire-drained")
    assert not any("injection" in line for line in observed), observed

    # A veto discards both queued messages; no message may escape the transaction.
    reload(
        r"""
ircd.on('preCommand', e => {
  if (!e.validated || e.command !== 'PING' || e.params[0] !== 'rss-deny') return;
  ircd.notice('#rss-messaging', 'rss-discard-notice');
  ircd.privmsg('#rss-messaging', 'rss-discard-privmsg');
  return {action:'deny'};
});
""",
        "admin",
    )
    caller.send("PING :rss-deny")
    own = caller.barrier("rss-denied-effects-drained")
    observed = other.barrier("rss-denied-observer-drained")
    assert not any("rss-discard-" in line for line in own + observed), own + observed

    # Queue against a live channel in validated preCommand, then PART removes its
    # last member before deferred flush. The server must skip the vanished target.
    vanishing = "#rss-vanishing"
    caller.send("JOIN " + vanishing)
    caller.barrier("rss-vanishing-joined")
    reload(
        r"""
ircd.on('preCommand', e => {
  if (e.validated && e.command === 'PART' && e.params[0] === '#rss-vanishing') {
    ircd.notice('#rss-vanishing', 'rss-vanished-notice');
    ircd.privmsg('#rss-vanishing', 'rss-vanished-privmsg');
  }
});
""",
        "admin",
    )
    caller.send("PART " + vanishing)
    own = caller.barrier("rss-vanished-channel-flushed")
    assert not any("rss-vanished-" in line and " PONG " not in line for line in own), own
    caller.send("JOIN " + vanishing)
    own += caller.barrier("rss-recreated-channel")
    assert not any("rss-vanished-notice" in line or "rss-vanished-privmsg" in line for line in own)
    caller.send("PART " + vanishing)
    caller.barrier("rss-recreated-cleanup")

    # The equivalent user lifetime transition uses an extra synthetic client so
    # the rest of the native integration suite retains its existing participants.
    host, port = caller.sock.getpeername()[:2]
    temporary = Client(host, port, "rssquitter")
    try:
        reload(
            r"""
ircd.on('preCommand', e => {
  if (e.validated && e.command === 'QUIT') {
    ircd.notice(e.userId, 'rss-quit-notice');
    ircd.privmsg(e.userId, 'rss-quit-privmsg');
  }
});
""",
            "admin",
        )
        temporary.send("QUIT :rss lifetime test")
        temporary.sock.settimeout(3)
        shutdown = temporary.buffer
        while True:
            chunk = temporary.sock.recv(4096)
            if not chunk:
                break
            shutdown += chunk
        assert b"rss-quit-notice" not in shutdown and b"rss-quit-privmsg" not in shutdown, shutdown
        caller.barrier("rss-quit-target-flushed")
    finally:
        temporary.close()
    reload("")
    caller.send("PART " + channel)
    other.send("PART " + channel)
    caller.barrier("rss-messaging-cleanup")
    other.barrier("rss-messaging-other-cleanup")
    print(
        "PASS: native local user/channel NOTICE and PRIVMSG, server origin, no duplicates, "
        "background timers, user authority, nonoperator RSS denial, veto rollback and disappearing targets"
    )


def exercise_http_poison(caller, reload):
    """Test-only loopback build: disabling a script cancels committed work."""
    import json
    import time

    from test_js_webhooks import Receiver

    for mode in ("postCommand", "timeout", "preCommand", "userPreMessage"):
        receiver = Receiver()
        try:
            source = (
                f"const base={json.dumps(receiver.url)};\n"
                + """
ircd.on('postCommand',e=>{
  if(e.command==='PING' && e.params[0]==='PRIME') {
    for(let i=0;i<2;i++) ircd.fetch(base+'/slow',{method:'POST',body:'inflight'});
    for(let i=0;i<2;i++) ircd.fetch(base+'/queued',{method:'POST',body:'prior'});
  }
});
"""
            )
            poison = "ircd.fetch(base+'/current',{method:'POST',body:'current'});"
            poison += "while(true){}" if mode == "timeout" else "throw Error('poison');"
            if mode == "userPreMessage":
                source += "ircd.on('userPreMessage',()=>{" + poison + "});"
            else:
                hook = "preCommand" if mode == "preCommand" else "postCommand"
                source += (
                    "ircd.on('"
                    + hook
                    + "',e=>{if(e.command==='PING'&&e.params[0]==='POISON'){"
                    + poison
                    + "}});"
                )
            reload(source, "admin")
            caller.send("JOIN #http-poison")
            caller.barrier("poison-joined")
            caller.send("PING PRIME")
            receiver.wait(2)
            caller.send(
                "PRIVMSG #http-poison :poison" if mode == "userPreMessage" else "PING POISON"
            )
            time.sleep(4.2)
            assert [post[0] for post in receiver.posts] == ["/slow", "/slow"], (
                mode,
                receiver.posts,
            )
        finally:
            receiver.close()
    print("PASS: poisoned scripts cancel queued HTTP and discard staged requests on hooks/timeout")


def main():
    """Run this focused suite without the longer native hook inventory tests."""
    import grp
    import os
    import pathlib
    import pwd
    import signal
    import socket
    import subprocess
    import tempfile
    import time

    run = pathlib.Path(os.environ["INSPIRCD_NATIVE"]).resolve()
    with tempfile.TemporaryDirectory(prefix="irc-rss-messaging-") as directory:
        path = pathlib.Path(directory)
        script = path / "hooks.js"
        script.write_text("")
        config = path / "inspircd.conf"
        logfile = path / "server.log"
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        username = pwd.getpwuid(os.getuid()).pw_name
        base = f'''<server name="js.test" description="Synthetic messaging tests" id="001" network="JSTest">
<admin name="Test" email="test@example.invalid">
<path moduledir="{run / "modules"}">
<bind address="127.0.0.1" port="{port}" type="clients">
<connect name="test" allow="127.0.0.1" maxlocal="20" maxglobal="20" fakelag="yes" threshold="100000">
<security user="{username}" group="{grp.getgrgid(os.getgid()).gr_name}">
<module name="javascript">
<log method="stdout" type="m_javascript MODULE CONFIG" level="normal">
<javascriptsettings timeoutms="200">
'''
        config.write_text(base + f'<javascript file="{script}">\n')
        clients = []
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
                caller = Client("127.0.0.1", port, "rsscaller")
                clients.append(caller)
                other = Client("127.0.0.1", port, "rssother")
                clients.append(other)

                def reload(source, authority="user"):
                    script.write_text(source)
                    config.write_text(
                        base + f'<javascript file="{script}" authority="{authority}">\n'
                    )
                    offset = logfile.stat().st_size
                    server.send_signal(signal.SIGHUP)
                    deadline = time.monotonic() + 15
                    while time.monotonic() < deadline:
                        output = logfile.read_text()[offset:]
                        assert server.poll() is None, output
                        if "JavaScript configuration loaded" in output:
                            return
                        if "Unable to read the configuration for m_javascript" in output:
                            raise AssertionError(output)
                        time.sleep(0.1)
                    raise AssertionError("Rehash did not complete: " + logfile.read_text()[offset:])

                if os.environ.get("RSS_HTTP_POISON_TEST"):
                    exercise_http_poison(caller, reload)
                else:
                    exercise_messaging(caller, other, reload)
            except Exception:
                print(logfile.read_text())
                raise
            finally:
                for client in clients:
                    client.close()
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)


def exercise_remote_messaging():
    """Verify user/channel routing over a real, isolated two-server link."""
    import grp
    import os
    import pathlib
    import pwd
    import socket
    import subprocess
    import tempfile
    import time

    run = pathlib.Path(os.environ["INSPIRCD_NATIVE"]).resolve()
    with tempfile.TemporaryDirectory(prefix="irc-rss-linked-") as directory:
        path = pathlib.Path(directory)
        ports = []
        reservations = []
        for _ in range(3):
            sock = socket.socket()
            sock.bind(("127.0.0.1", 0))
            ports.append(sock.getsockname()[1])
            reservations.append(sock)
        script = path / "hooks.js"
        script.write_text(r"""
let remoteId;
ircd.on('userJoin', e => {
  if (e.memb.user.nick.text === 'rssremote') remoteId = e.memb.user.id;
});
ircd.on('postCommand', e => {
  if (e.command !== 'PING' || e.params[0] !== 'rss-send-remote') return;
  if (!remoteId) throw Error('remote UUID missing');
  ircd.notice(remoteId, 'rss-remote-user-notice');
  ircd.privmsg(remoteId, 'rss-remote-user-privmsg');
  ircd.notice('#rss-remote', 'rss-remote-channel-notice');
  ircd.privmsg('#rss-remote', 'rss-remote-channel-privmsg');
});
""")
        processes, logs, clients = [], [], []
        try:
            for index, name in enumerate(("rss-a.test", "rss-b.test")):
                config = path / f"{index}.conf"
                peer = "rss-b.test" if index == 0 else "rss-a.test"
                text = f'''<server name="{name}" description="Synthetic linked messaging tests" id="00{index + 1}" network="JSTest">
<admin name="Test" email="test@example.invalid">
<pid file="{path / f"{index}.pid"}">
<bind address="127.0.0.1" port="{ports[index]}" type="clients">
<connect name="test" allow="127.0.0.1" maxlocal="20" maxglobal="20" fakelag="yes" threshold="100000">
<security user="{pwd.getpwuid(os.getuid()).pw_name}" group="{grp.getgrgid(os.getgid()).gr_name}">
<module name="spanningtree">
<module name="javascript">
<log method="stdout" type="*" level="normal">
<link name="{peer}" ipaddr="127.0.0.1" port="{ports[2]}" allowmask="127.0.0.1" sendpass="synthetic-loopback-link" recvpass="synthetic-loopback-link">
'''
                if index == 0:
                    text += f'<bind address="127.0.0.1" port="{ports[2]}" type="servers">\n'
                    text += f'<javascript file="{script}" authority="admin">\n'
                else:
                    text += '<autoconnect bootperiod="1s" period="1s" server="rss-a.test">\n'
                config.write_text(text)
            for sock in reservations:
                sock.close()
            for index in range(2):
                log = (path / f"{index}.log").open("w")
                logs.append(log)
                processes.append(
                    subprocess.Popen(
                        [
                            str(run / "bin/inspircd"),
                            "--nofork",
                            "--config",
                            str(path / f"{index}.conf"),
                        ],
                        cwd=run,
                        stdout=log,
                        stderr=subprocess.STDOUT,
                    )
                )
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                assert all(process.poll() is None for process in processes), "Linked daemon exited"
                if all(
                    "Received end of netburst" in (path / f"{i}.log").read_text() for i in range(2)
                ):
                    break
                time.sleep(0.1)
            else:
                raise AssertionError("Synthetic servers did not link")
            caller = Client("127.0.0.1", ports[0], "rsslocal")
            clients.append(caller)
            remote = Client("127.0.0.1", ports[1], "rssremote")
            clients.append(remote)
            caller.send("JOIN #rss-remote")
            caller.barrier("rss-local-joined")
            remote.send("JOIN #rss-remote")
            remote.barrier("rss-remote-joined")
            caller.until(lambda line: line.startswith(":rssremote!") and " JOIN " in line)
            caller.send("PING :rss-send-remote")
            received = remote.until(lambda line: line.endswith(":rss-remote-channel-privmsg"))
            received += remote.barrier("rss-remote-duplicates")
            for command, target, suffix in [
                ("NOTICE", remote.nick, "rss-remote-user-notice"),
                ("PRIVMSG", remote.nick, "rss-remote-user-privmsg"),
                ("NOTICE", "#rss-remote", "rss-remote-channel-notice"),
                ("PRIVMSG", "#rss-remote", "rss-remote-channel-privmsg"),
            ]:
                matches = _messages(received, command, target, suffix)
                assert len(matches) == 1, (command, target, received)
                assert matches[0].startswith(":rss-a.test "), matches
            received = caller.until(lambda line: line.endswith(":rss-remote-channel-privmsg"))
            received += caller.barrier("rss-local-duplicates")
            assert not any("rss-remote-user-" in line for line in received), received
            for command, suffix in [
                ("NOTICE", "rss-remote-channel-notice"),
                ("PRIVMSG", "rss-remote-channel-privmsg"),
            ]:
                assert len(_messages(received, command, "#rss-remote", suffix)) == 1, received
            print(
                "PASS: real linked-server user/channel NOTICE and PRIVMSG, server origin and no duplicates"
            )
        except Exception:
            for index in range(2):
                logfile = path / f"{index}.log"
                if logfile.exists():
                    print(logfile.read_text())
            raise
        finally:
            for sock in reservations:
                sock.close()
            for client in clients:
                client.close()
            for process in processes:
                process.terminate()
            for process in processes:
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            for log in logs:
                log.close()


if __name__ == "__main__":
    main()
    if not __import__("os").environ.get("RSS_HTTP_POISON_TEST"):
        exercise_remote_messaging()
