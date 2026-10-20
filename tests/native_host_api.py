#!/usr/bin/env python3
"""Exercise host APIs in a disposable loopback InspIRCd daemon, never production.

For the HTTP self-unload regression, separately compile the test module with
-DJS_ASYNC_HTTP_TEST_LOOPBACK and set INSPIRCD_HTTP_TEST_LOOPBACK=1. Never enable
that transport bypass in production builds. A positive-control POST must arrive
before the negative stale-request assertion can pass.
"""

import grp
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
from replay.native import exercise_heap_replay
from test_js_webhooks import Receiver

ROOT = Path(__file__).resolve().parent.parent

SOURCE = (
    (ROOT / "tests/fixtures/native_utilities.js").read_text()
    + r"""
function assert(value, label) { if (!value) throw Error(label); }
ircd.registerCommand('JSHOST', 1, 3, false, e => {
 const action = e.params[0], id = e.userId;
 const other = ircd.userByNick('hostother');
 switch(action) {
 case 'query': {
   const me = ircd.userByNick('hostcaller');
   const channel = ircd.channelByName('#host');
   assert(me.id === id && me.local && me.username && me.realname, 'user snapshot');
   assert(channel.name === '#host' && channel.members.some(m => m.id === id), 'channel snapshot');
   assert(ircd.channels().includes('#host'), 'channel list');
   assert(ircd.userByNick('no-such-nick') === null && ircd.channelByName('#absent') === null, 'missing');
   ircd.notice(id, 'host-query-ok'); break;
 }
 case 'join': ircd.join(other.id, '#host', ''); break;
 case 'part': ircd.part(other.id, '#host', 'host-part'); break;
 case 'kick': ircd.kick(other.id, '#host', 'host-kick'); break;
 case 'topic': ircd.setTopic('#host', 'host-topic'); break;
 case 'mode': ircd.setModes('#host', '+m'); break;
 case 'identity': ircd.setHost(other.id, 'changed.example'); ircd.setUsername(other.id, 'changedu'); ircd.setRealname(other.id, 'Changed Realname'); break;
 case 'identityQuery': {
   assert(other.hostname === 'changed.example' && other.username === 'changedu' && other.realname === 'Changed Realname', 'identity changes');
   ircd.notice(id, 'host-identity-ok'); break;
 }
 case 'timer': {
   let count = 0;
   const repeated = ircd.setTimer(1, true, () => { if (++count === 2) { ircd.clearTimer(repeated); ircd.notice(id, 'host-repeat-ok'); } });
   const canceled = ircd.setTimer(1, false, () => ircd.notice(id, 'host-must-not-fire'));
   assert(ircd.clearTimer(canceled) === true && ircd.clearTimer(canceled) === false, 'timer cancellation');
   ircd.setTimer(1, false, () => ircd.notice(id, 'host-timer-ok'));
   break;
 }
 case 'ban': ircd.addBan('Q', 'hostbanned', '60', 'native host test'); break;
 case 'unban': ircd.removeBan('Q', 'hostbanned'); break;
 case 'load': ircd.loadModule('m_alltime.so'); break;
 case 'unload': ircd.unloadModule('m_alltime.so'); break;
 case 'selfUnload': ircd.unloadModule('m_javascript.so'); ircd.setTopic('#host', 'stale-effect'); break;
 case 'disconnect': ircd.disconnect(other.id, 'host-disconnect'); break;
 case 'rehash': ircd.rehash(); break;
 case 'shutdown': ircd.shutdown(); break;
 case 'rollback': ircd.setTopic('#host', 'must-not-apply'); throw Error('rollback');
 }
});
ircd.registerCommand('JSOPER', 0, 0, true, e => ircd.notice(e.userId, 'must-not-oper'));
"""
)


def exercise_native_host(caller, other, reload):
    """Reusable within the full native test suite; caller must use hostcaller nick."""
    reload(SOURCE, "admin")
    caller.send("JOIN #host")
    caller.barrier("host-ready")
    caller.send("JSHOST query")
    caller.until(lambda line: line.endswith(":host-query-ok"))
    caller.send("JSOPER")
    lines = caller.barrier("operator-denial")
    assert any(" 481 " in line for line in lines), lines
    assert not any(line.endswith(":must-not-oper") for line in lines), lines
    caller.send("JSHOST")
    lines = caller.barrier("arity-denial")
    assert any(" 461 " in line for line in lines), lines
    for action, marker in [
        ("join", " JOIN "),
        ("part", ":host-part"),
        ("join", " JOIN "),
        ("kick", ":host-kick"),
    ]:
        caller.send("JSHOST " + action)
        other.until(lambda line, marker=marker: marker in line and "#host" in line)
    caller.send("JSHOST topic")
    caller.until(lambda line: " TOPIC #host " in line and line.endswith(":host-topic"))
    caller.send("JSHOST mode")
    lines = caller.barrier("mode-result")
    assert any(" MODE #host " in line and "+m" in line for line in lines), lines
    caller.send("JSHOST identity")
    caller.barrier("identity-set")
    caller.send("JSHOST identityQuery")
    caller.until(lambda line: line.endswith(":host-identity-ok"))
    caller.send("JSHOST timer")
    lines = caller.until(lambda line: line.endswith(":host-repeat-ok"), timeout=20)
    assert any(line.endswith(":host-timer-ok") for line in lines), lines
    assert not any(line.endswith(":host-must-not-fire") for line in lines), lines
    caller.send("JSHOST ban")
    caller.barrier("ban-applied")
    other.send("NICK hostbanned")
    lines = other.barrier("ban-result")
    assert any(" 432 " in line for line in lines), lines
    caller.send("JSHOST unban")
    caller.barrier("ban-removed")
    other.send("NICK hostbanned")
    other.until(lambda line: " NICK " in line and line.endswith(":hostbanned"))
    other.nick = "hostbanned"
    other.send("NICK hostother")
    other.until(lambda line: " NICK " in line and line.endswith(":hostother"))
    other.nick = "hostother"
    caller.send("JSHOST rollback")
    caller.barrier("rollback-finished")
    caller.send("TOPIC #host")
    lines = caller.until(lambda line: " 332 " in line)
    assert any(line.endswith(":host-topic") for line in lines), lines
    reload(SOURCE, "admin")
    caller.send("JSHOST query")
    caller.until(lambda line: line.endswith(":host-query-ok"))
    # User-authority scripts cannot invoke query/admin/command/timer APIs.
    for expression in [
        "ircd.join(e.userId,'#forbidden','')",
        "ircd.part(e.userId,'#host','forbidden')",
        "ircd.kick(e.userId,'#host','forbidden')",
        "ircd.setTopic('#host','forbidden')",
        "ircd.setModes('#host','-m')",
        "ircd.setHost(e.userId,'forbidden.example')",
        "ircd.setUsername(e.userId,'forbidden')",
        "ircd.setRealname(e.userId,'Forbidden')",
        "ircd.disconnect(e.userId,'forbidden')",
        "ircd.removeBan('Q','hostcaller')",
        "ircd.clearTimer(1)",
        "ircd.channels()",
        "ircd.userByNick('hostcaller')",
        "ircd.channelByName('#host')",
        "ircd.setTimer(1,false,()=>{})",
        "ircd.shutdown()",
        "ircd.rehash()",
        "ircd.loadModule('m_alltime.so')",
        "ircd.unloadModule('m_javascript.so')",
        "ircd.addBan('Q','hostcaller','60','forbidden')",
        "ircd.registerCommand('DENIED',0,0,false,()=>{})",
    ]:
        reload(
            "ircd.on('postCommand', e => { if(e.command === 'PING' && e.params[0] === 'host-denied') { let denied=false; try { "
            + expression
            + "; } catch (_) { denied=true; } ircd.notice(e.userId, denied ? 'host-denied-ok' : 'host-denied-FAIL'); }});",
            "user",
        )
        caller.send("PING :host-denied")
        caller.until(lambda line: line.endswith(":host-denied-ok"))
    reload(
        r"""
let recipient = '';
const keep = ircd.setTimer(4, false, () => ircd.notice(recipient, 'timer-restored'));
ircd.on('preCommand', e => {
 if (!e.validated && e.command === 'PING' && e.params[0] === 'timer-veto') {
  recipient = e.userId;
  ircd.clearTimer(keep);
  ircd.setTimer(1, false, () => ircd.notice(recipient, 'timer-must-rollback'));
 }
});
""",
        "admin",
        "ircd.on('preCommand', e => { if (!e.validated && e.command === 'PING' && e.params[0] === 'timer-veto') return {action:'deny'}; });",
    )
    caller.send("PING :timer-veto")
    lines = caller.until(lambda line: line.endswith(":timer-restored"), timeout=20)
    assert not any(line.endswith(":timer-must-rollback") for line in lines), lines
    reload(SOURCE, "admin")
    # Module registration is observable as command presence without granting oper.
    caller.send("JSHOST load")
    caller.barrier("module-loaded")
    caller.send("ALLTIME")
    lines = caller.barrier("module-present")
    assert any(" 481 " in line for line in lines), lines
    caller.send("JSHOST unload")
    caller.barrier("module-unloaded")
    caller.send("ALLTIME")
    lines = caller.barrier("module-absent")
    assert any(" 421 " in line and "ALLTIME" in line for line in lines), lines
    caller.send("JSHOST disconnect")
    other.until(lambda line: "ERROR" in line and "host-disconnect" in line)
    print(
        "PASS: native queries, commands, timers, channel/user mutations, Q-line and module lifecycle, cross-script rollback and authority"
    )


def exercise_http_poison(caller, reload):
    """Prior committed queue and current staged effects die with their script."""
    for mode in ("command-throw", "command-timeout", "preCommand", "backgroundTimer", "hostTimer"):
        receiver = Receiver()
        try:
            url = json.dumps(receiver.url)
            poison = "ircd.fetch(base+'/current',{method:'POST',body:'current'});"
            poison += "while(true){}" if mode == "command-timeout" else "throw Error('poison');"
            source = (
                f"const base={url};\n"
                + """
ircd.registerCommand('HTTPPRIME',0,0,false,e=>{
  for(let i=0;i<2;i++) ircd.fetch(base+'/slow',{method:'POST',body:'inflight'});
  for(let i=0;i<2;i++) ircd.fetch(base+'/queued',{method:'POST',body:'prior'});
});
"""
            )
            if mode.startswith("command-"):
                source += "ircd.registerCommand('HTTPPOISON',0,0,false,e=>{" + poison + "});"
            elif mode == "preCommand":
                source += (
                    "ircd.on('preCommand',e=>{if(e.command==='PING'&&e.params[0]==='POISON'){"
                    + poison
                    + "}});"
                )
            elif mode == "backgroundTimer":
                source += (
                    "let armed=false;ircd.registerCommand('HTTPPOISON',0,0,false,e=>{armed=true;});"
                )
                source += "ircd.on('backgroundTimer',()=>{if(armed){" + poison + "}});"
            else:
                source += (
                    "ircd.registerCommand('HTTPPOISON',0,0,false,e=>ircd.setTimer(1,false,()=>{"
                    + poison
                    + "}));"
                )
            reload(source, "admin")
            caller.send("HTTPPRIME")
            receiver.wait(2)
            caller.send("PING POISON" if mode == "preCommand" else "HTTPPOISON")
            time.sleep(4.2)  # Both already-sent /slow requests have completed.
            assert [p[0] for p in receiver.posts] == ["/slow", "/slow"], (mode, receiver.posts)
        finally:
            receiver.close()
    print(
        "PASS: poison cancels prior queued HTTP and current staged HTTP on throw/timeout/hooks/timers"
    )


def exercise_http_self_unload(caller, reload):
    """Requires a test-only module built with JS_ASYNC_HTTP_TEST_LOOPBACK.

    A successful control POST proves the transport is enabled; otherwise a
    rejected URL could make the stale-effect assertion pass vacuously.
    """
    receiver = Receiver()
    try:
        url = json.dumps(receiver.url + "/native-unload")
        reload(
            SOURCE
            + "\nircd.registerCommand('HTTPBEFORE',0,0,false,e=>ircd.fetch("
            + url
            + ",{method:'POST',body:'control'}));"
            + "\nircd.registerCommand('HTTPUNLOAD',0,0,false,e=>{ircd.unloadModule('m_javascript.so');ircd.fetch("
            + url
            + ",{method:'POST',body:'must-not-send'});});",
            "admin",
        )
        caller.send("HTTPBEFORE")
        caller.barrier("http-control-queued")
        with receiver.condition:
            assert receiver.condition.wait_for(lambda: len(receiver.posts) >= 1, timeout=10), (
                "Control POST missing; module must be built with JS_ASYNC_HTTP_TEST_LOOPBACK"
            )
            assert len(receiver.posts) == 1 and receiver.posts[0][1] == b"control", receiver.posts
        caller.send("HTTPUNLOAD")
        caller.barrier("http-self-unloaded")
        caller.send("HTTPBEFORE")
        lines = caller.barrier("http-command-removed")
        assert any(" 421 " in line and "HTTPBEFORE" in line for line in lines), lines
        with receiver.condition:
            assert not receiver.condition.wait_for(lambda: len(receiver.posts) > 1, timeout=2), (
                receiver.posts
            )
        print("PASS: self-unload suppresses subsequent POST; fixture positive control received")
    finally:
        receiver.close()


def main():
    run = Path(os.environ["INSPIRCD_NATIVE"]).resolve()
    with tempfile.TemporaryDirectory(prefix="irc-native-host-") as directory:
        path = Path(directory)
        script = path / "host.js"
        script.write_text(SOURCE)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        base = f'''<server name="host.test" description="Disposable host API tests" id="001" network="HostTest">
<admin name="Test" email="test@example.invalid">
<bind address="127.0.0.1" port="{port}" type="clients">
<connect name="test" allow="127.0.0.1" maxlocal="20" maxglobal="20" fakelag="yes" threshold="100000">
<security user="{pwd.getpwuid(os.getuid()).pw_name}" group="{grp.getgrgid(os.getgid()).gr_name}">
<module name="javascript">
<log method="stdout" type="m_javascript MODULE CONFIG" level="normal">
<javascriptsettings timeoutms="200">
'''
        config = path / "inspircd.conf"
        config.write_text(base + f'<javascript file="{script}" authority="admin">\n')
        with (path / "server.log").open("w") as log:
            server = subprocess.Popen(
                [str(run / "bin/inspircd"), "--nofork", "--config", str(config)],
                cwd=run,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            clients = []
            try:
                time.sleep(0.4)
                assert server.poll() is None, (path / "server.log").read_text()
                caller = Client("127.0.0.1", port, "hostcaller")
                clients.append(caller)
                other = Client("127.0.0.1", port, "hostother")
                clients.append(other)

                def reload(source, authority="admin", second_source=None):
                    script.write_text(source)
                    tags = f'<javascript file="{script}" authority="{authority}">\n'
                    if second_source is not None:
                        second = path / "second.js"
                        second.write_text(second_source)
                        tags += f'<javascript file="{second}" authority="{authority}">\n'
                    config.write_text(base + tags)
                    offset = (path / "server.log").stat().st_size
                    server.send_signal(signal.SIGHUP)
                    deadline = time.monotonic() + 15
                    while time.monotonic() < deadline:
                        output = (path / "server.log").read_text()[offset:]
                        if "JavaScript configuration loaded" in output:
                            return
                        if "Unable to read the configuration for m_javascript" in output:
                            raise AssertionError(output)
                        assert server.poll() is None, output
                        time.sleep(0.1)
                    raise AssertionError("Rehash did not complete: " + output)

                exercise_heap_replay(caller, reload)
                print("PASS: real V8 Map and closure replay")
                exercise_native_host(caller, other, reload)
                # Rehash itself is requested through the host API and validated by new config.
                script.write_text(
                    SOURCE
                    + "\nircd.registerCommand('AFTERREHASH',0,0,false,e=>ircd.notice(e.userId,'host-rehash-ok'));"
                )
                caller.send("JSHOST rehash")
                time.sleep(1.5)
                caller.send("AFTERREHASH")
                caller.until(lambda line: line.endswith(":host-rehash-ok"))
                # Self-unload must invalidate effects queued behind it, without crashing.
                caller.send("JSHOST selfUnload")
                caller.barrier("self-unloaded")
                caller.send("JSHOST query")
                lines = caller.barrier("removed-command")
                assert any(" 421 " in line and "JSHOST" in line for line in lines), lines
                caller.send("TOPIC #host")
                lines = caller.until(lambda line: " 332 " in line)
                assert any(line.endswith(":host-topic") for line in lines), lines
                print("PASS: native rehash and self-unload discard stale queued effects")
                reload(SOURCE, "admin")
                if os.environ.get("INSPIRCD_HTTP_TEST_LOOPBACK") == "1":
                    exercise_http_poison(caller, reload)
                    exercise_http_self_unload(caller, reload)
                    reload(SOURCE, "admin")
                # Shutdown is tested only after every other check in this disposable process.
                caller.send("JSHOST shutdown")
                server.wait(timeout=15)
                assert server.returncode == 0
                print("PASS: admin shutdown of isolated test daemon")
            except BaseException:
                print((path / "server.log").read_text())
                raise
            finally:
                for client in clients:
                    client.close()
                if server.poll() is None:
                    server.terminate()
                    server.wait(timeout=10)


if __name__ == "__main__":
    main()
