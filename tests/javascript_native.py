#!/usr/bin/env python3
"""Synthetic loopback integration. Requires a built native InspIRCd installation.
Run: INSPIRCD_NATIVE=/path/to/run tests/javascript.sh
The installation must already contain m_javascript.so. No existing config is read.
"""

import grp
import json
import os
import pathlib
import pwd
import signal
import socket
import subprocess
import tempfile
import time

from precommand_client import Client
from rss_native import exercise_messaging, exercise_remote_messaging
from test_js_webhooks import Receiver
from wchat_nocodb_native import exercise_wchat_nocodb

ROOT = pathlib.Path(__file__).resolve().parent.parent
RUN = pathlib.Path(os.environ["INSPIRCD_NATIVE"]).resolve()
GOOD = (
    (ROOT / "tests/fixtures/native_utilities.js").read_text()
    + r"""
if (!ircd.match('hello', 'h*') || !ircd.matchCIDR('192.0.2.4', '192.0.2.0/24') ||
    !ircd.isNick('ValidNick') || !ircd.isChannel('#valid') ||
    ircd.stripColor('\x0304red') !== 'red' || ircd.time() <= 0) throw Error('native utilities');
const wasm = new WebAssembly.Instance(new WebAssembly.Module(ircd.wasm('answer')), {});
const users = new Map();
ircd.on('preCommand', e => {
  if (e.validated) return;
  const user = ircd.userById(e.userId);
  if (user) users.set(user.nick, e.userId);
  if (e.command === 'PING' && e.params[0] === 'rewrite')
    return {action:'pass', command:'VERSION', params:[]};
  if (e.command === 'XALIAS') return {action:'pass',command:'INFO',params:[]};
  if (e.command === 'XLOOP') return {action:'pass',command:'TIME',params:[]};
  if (e.command === 'XSERVER') return {action:'pass',command:'WHOISNOTICE',params:[e.userId,e.userId]};
  if (e.command === 'XOPERONLY') return {action:'pass',command:'DIE',params:['synthetic']};
  if (e.command === 'XWASM')
    return {action:'pass', command:'PING', params:[String(wasm.exports.answer())]};
  if (e.command === 'XBYTES') {
    if (e.params[0] !== null || e.paramBytes[0][0] !== 255) throw Error('bytes');
    return {action:'pass', command:'PING', params:[e.paramBytes[0]]};
  }
  if (e.command === 'PING' && e.params[0] === 'deny') {
    ircd.notice(e.userId, 'must-discard'); return {action:'deny'};
  }
  if (e.command === 'XSPOOF') {
    ircd.rename((users.get('jsother') || users.get('adminrenamed')), 'changed'); return {action:'deny'};
  }
  if (e.command === 'XRENAME') { ircd.rename(e.userId, 'selfrenamed'); return {action:'deny'}; }
  if (e.command === 'XSELF') { ircd.rename(e.userId, 'selfrenamed'); }
  if (e.command === 'XNOTICE') { ircd.notice(e.userId, 'owned: café two  spaces'); }
  if (e.command === 'XADMIN') { ircd.rename(users.get('jsother'), 'adminrenamed'); }
});
ircd.on('postCommand', e => {
  if (e.command === 'WHOISNOTICE') ircd.notice(e.userId, 'forbidden-server-effect');
  if (e.command === 'OPER') ircd.notice(e.userId, 'oper-post:' + e.result);
  if (e.command === 'VERSION') ircd.notice(e.userId, 'post:' + e.result);
});
"""
)


HOOKS = json.loads((ROOT / "bindings/module_hooks.json").read_text())["hooks"]
FULL_HOOKS = (
    "const hookNames = "
    + json.dumps([h["js"] for h in HOOKS])
    + r""";
let selected = '*', action = 'pass', remaining = 0;
const seen = new Set(), snapshots = new Map();
for (const stage of hookNames) ircd.on(stage, e => {
  if (e.stage !== stage) throw Error('wrong stage');
  const control = stage === 'preCommand' && e.command === 'JSHOOKPROBE';
  if (control && e.params[0] !== 'result') {
    selected = e.params[0] || '*'; action = e.params[1] || 'pass'; remaining = 1;
    seen.clear(); snapshots.clear();
  }
  seen.add(stage); snapshots.set(stage, e);
  if (stage === 'postCommand' && e.command === 'JSHOOKPROBE' && e.params[0] === '*') {
    if (hookNames.some(name => !seen.has(name))) throw Error('missing hook');
    const preJoin = snapshots.get('userPreJoin');
    if (preJoin.chan !== null || preJoin.user.id !== e.userId || !preJoin.user.local) throw Error('join snapshot');
    if (snapshots.get('preRehash').user !== null || snapshots.get('preRehash').userId !== null) throw Error('server actor');
    if (snapshots.get('userPreMessage').target.name.text !== '#jsprobe') throw Error('message target');
    if (snapshots.get('userPreMessage').details.text.text !== 'probe') throw Error('message text');
    if (snapshots.get('mode').changelist.items[0].mode.text !== 'i') throw Error('mode snapshot');
    if (snapshots.get('buildNeighborList').exceptions.items[0].user.id !== e.userId) throw Error('neighbors');
    if (snapshots.get('acceptConnection').fd !== -1 || !snapshots.get('acceptConnection').sock) throw Error('listener');
    if (snapshots.get('preChangeConnectClass').errnum.code !== 999) throw Error('numeric');
    if (snapshots.get('serviceAdd').service.name.text !== 'JSHOOKPROBE') throw Error('service');
    if (snapshots.get('cleanup').item.value.id !== e.userId) throw Error('cleanup');
    // These C++ stack values have already been destroyed: all nested copies must survive.
    const line = snapshots.get('addLine').line;
    if (line.type.text !== 'K' || line.reason.text !== 'probe' || line.reason.truncated) throw Error('owned line');
    line.reason.bytes[0] = 0;
    if (snapshots.get('delLine').line.reason.bytes[0] !== 112) throw Error('independent bytes');
    ircd.notice(e.userId, 'full-hook-inventory:75');
  }
  if (!control && selected === stage && remaining > 0) {
    --remaining;
    if (action === 'error') throw Error('synthetic hook error');
    if (action === 'timeout') while (true) {}
    if (action === 'promise') return Promise.resolve({action: 'pass'});
    if (action === 'malformed') return {action: 'pass', typo: true};
    return {action};
  }
});
"""
)


def exercise_module_hooks(caller, reload):
    caller.send("JOIN #jsprobe")
    caller.barrier("probe-channel")
    reload(FULL_HOOKS)
    caller.send("JSHOOKPROBE * pass")
    caller.until(lambda line: line.endswith(":full-hook-inventory:75"))
    decisions = [h for h in HOOKS if h["result"] == "ModResult"]
    for authority, action in (("user", "pass"), ("user", "deny"), ("admin", "allow")):
        for hook in decisions:
            if action == "allow" and not hook["allow"]:
                continue
            reload(FULL_HOOKS, authority)
            caller.send(f"JSHOOKPROBE {hook['js']} {action}")
            caller.send("JSHOOKPROBE result")
            caller.until(
                lambda line, fragment=f"probe-result:{hook['js']}:{action} ": fragment in line
            )
    # Invalid decisions, asynchronous returns, exceptions and timeouts poison the
    # isolate. Read native results only after a successful reload, not through a
    # potentially fail-closed userWrite callback.
    failure_cases = [
        ("userPreNick", "error", "user", "deny"),
        ("userPreMessage", "timeout", "user", "deny"),
        ("checkPassword", "promise", "user", "deny"),
        ("preMode", "malformed", "user", "deny"),
        ("acceptConnection", "allow", "admin", "deny"),
        ("userPreJoin", "allow", "user", "deny"),
        ("userPreQuit", "error", "user", "pass"),
        ("channelPreDelete", "error", "user", "pass"),
        ("kill", "error", "user", "pass"),
    ]
    for stage, action, authority, expected in failure_cases:
        reload(FULL_HOOKS, authority)
        caller.send(f"JSHOOKPROBE {stage} {action}")
        time.sleep(0.4)  # callback must have finished before rehash
        reload("")
        caller.send("JSHOOKPROBE result")
        caller.until(lambda line, fragment=f"probe-result:{stage}:{expected} ": fragment in line)
    # Real traffic, not only direct driver calls: snapshot changes cannot rewrite
    # native state, and a denied lifecycle action must leave that state intact.
    reload(r"""
ircd.on('userPreMessage', e => { if (e.details.text.text === 'hook-block') return {action:'deny'}; });
ircd.on('userPreNick', e => { if (e.newnick.text === 'hookblocked') return {action:'deny'}; e.newnick.text='not-a-native-rewrite'; });
ircd.on('userJoin', e => { if (e.memb.channel.name.text === '#jslive') ircd.notice(e.userId,'real-join'); });
ircd.on('userPart', e => { if (e.memb.channel.name.text === '#jslive') ircd.notice(e.userId,'real-part'); });
""")
    caller.send("JOIN #jslive")
    caller.until(lambda line: line.endswith(":real-join"))
    caller.send("NICK hookblocked")
    caller.barrier("nick-denied")
    assert irc_nick(caller, caller.nick)
    caller.send("NICK hooksafe")
    caller.until(lambda line: " NICK " in line and line.endswith(":hooksafe"))
    caller.nick = "hooksafe"
    caller.send("PRIVMSG hooksafe :hook-block")
    lines = caller.barrier("message-denied")
    assert not any(" PRIVMSG " in line and line.endswith(":hook-block") for line in lines), lines
    caller.send("PRIVMSG hooksafe :hook-pass")
    caller.until(lambda line: " PRIVMSG " in line and line.endswith(":hook-pass"))
    caller.send("PART #jslive :probe")
    caller.until(lambda line: line.endswith(":real-part"))
    reload(GOOD)
    print(
        "PASS: all 75 native Module adapters, every ModResult pass/deny/admin-allow, hook failures, and real lifecycle policy"
    )


def exercise_validated_commands(caller, reload):
    reload(r"""
ircd.on('preCommand', e => {
  if (e.command === 'XVALIDATED' && !e.validated)
    return {action:'pass', command:'PING', params:['validated-deny']};
  if (e.command === 'PING' && e.params[0] === 'validated-deny' && e.validated)
    return {action:'deny'};
  if (e.command === 'PING' && e.params[0] === 'phase-probe')
    ircd.notice(e.userId, 'phase:' + String(e.validated));
});
""")
    caller.send("PING :phase-probe")
    lines = caller.barrier("phases-finished")
    assert sum(line.endswith(":phase:false") for line in lines) == 1, lines
    assert sum(line.endswith(":phase:true") for line in lines) == 1, lines
    caller.send("PING :validated-deny")
    caller.send("XVALIDATED")
    lines = caller.barrier("validated-denials-finished")
    assert not any(" PONG " in line and line.endswith("validated-deny") for line in lines), lines
    # Rewriting after core validation must fail closed, rather than bypass the
    # completed command permission/arity checks.
    for result in (
        "{action:'pass', command:'DIE', params:['synthetic']}",
        "Object.assign(Object.create({command:'PING',params:['late']}),{action:'pass'})",
        "new Proxy({action:'pass'}, {})",
    ):
        reload(
            "ircd.on('preCommand', e => { if(e.validated && e.command === 'PING') return "
            + result
            + "; });"
        )
        caller.send("PING :invalid-validated-rewrite")
        assert_silent(caller)
    reload(GOOD)
    caller.barrier("validated-rewrite-recovered")
    print(
        "PASS: both preCommand phases, validated deny, rewritten-command validation and late rewrite rejection"
    )


def exercise_native_mutations(caller, other, reload):
    # Inspect actual native reference state after a direct hook call, including
    # deny rollback (which ordinary delivery tests alone cannot establish).
    for action, expected in [("pass", "probe-rewritten"), ("deny", "probe")]:
        source = "ircd.on('userPreMessage', e => ({action:'pass',patch:{text:'probe-rewritten'}}));"
        if action == "deny":
            source += "ircd.on('userPreMessage', e => ({action:'deny'}));"
        reload(source)
        caller.send("JSHOOKPROBE userPreMessage")
        caller.send("JSHOOKPROBE result")
        lines = caller.until(lambda line: ":probe-result:" in line)
        assert any("message-text:" + expected + " " in line for line in lines), lines
    # User patches cannot restore native-exempt recipients; admin patches may.
    for authority, count in [("user", 1), ("admin", 0)]:
        reload(
            "ircd.on('userPreMessage', e => ({action:'pass',patch:{exemptions:[]}}));", authority
        )
        caller.send("JSHOOKPROBE userPreMessage")
        caller.send("JSHOOKPROBE result")
        lines = caller.until(lambda line: ":probe-result:" in line)
        assert any("message-exemptions:" + str(count) + " " in line for line in lines), lines
    for authority, expected in [("user", "original"), ("admin", "changed")]:
        reload(
            "ircd.on('userPreMessage', e => ({action:'pass',patch:{tagsOut:[{name:'+probe',value:'changed'}]}}));",
            authority,
        )
        caller.send("JSHOOKPROBE userPreMessage")
        time.sleep(0.3)
        reload("")  # Inspect after reloading a context poisoned by denied authority.
        caller.send("JSHOOKPROBE result")
        lines = caller.until(lambda line: ":probe-result:" in line)
        assert any("message-tag:" + expected + " " in line for line in lines), lines
    value = r"x;fake=y with\slash"
    reload(
        "ircd.on('userPreMessage', e => ({action:'pass',patch:{tagsOut:[{name:'+probe',value:"
        + json.dumps(value)
        + "}]}}));",
        "admin",
    )
    caller.send("JSHOOKPROBE userPreMessage")
    caller.send("JSHOOKPROBE result")
    lines = caller.until(lambda line: ":probe-result:" in line)
    assert any(r"message-tag:x\:fake=y\swith\\slash " in line for line in lines), lines
    for authority, expected in [("user", "probe"), ("admin", "v")]:
        reload("ircd.on('userPreJoin', e => ({action:'pass',patch:{privs:'v'}}));", authority)
        caller.send("JSHOOKPROBE userPreJoin")
        time.sleep(0.3)
        reload("")
        caller.send("JSHOOKPROBE result")
        lines = caller.until(lambda line: ":probe-result:" in line)
        assert any("join-privileges:" + expected + " " in line for line in lines), lines
    for authority, patch, expected in (
        ("admin", "{code:499,params:['custom-reason']}", "499"),
        ("admin", "null", "none"),
        ("user", "{code:499,params:['custom-reason']}", "999"),
        ("admin", "{code:399,params:['bad']}", "999"),
        ("admin", "{code:600,params:['bad']}", "999"),
    ):
        reload(
            "ircd.on('preChangeConnectClass', e => ({action:'pass',patch:{errnum:"
            + patch
            + "}}));",
            authority,
        )
        caller.send("JSHOOKPROBE preChangeConnectClass")
        time.sleep(0.3)
        reload("")
        caller.send("JSHOOKPROBE result")
        lines = caller.until(lambda line: ":probe-result:" in line)
        assert any("connect-numeric:" + expected + " " in line for line in lines), lines
        if expected == "499":
            assert any("connect-reason:custom-reason " in line for line in lines), lines
    reload(
        "ircd.on('userPreMessage', e => ({action:'pass',patch:{text:'partial',echo:'invalid'}}));"
    )
    caller.send("JSHOOKPROBE userPreMessage")
    caller.send("JSHOOKPROBE result")
    lines = caller.until(lambda line: ":probe-result:" in line)
    assert any("message-text:probe " in line for line in lines), lines
    reload("")
    caller.send("JOIN #patches")
    caller.barrier("patch-caller-joined")
    other.send("JOIN #patches")
    other.barrier("patch-other-joined")
    reload(r"""
let otherId;
ircd.on('preCommand', e => {
  if (!e.validated && e.command === 'PING' && e.params[0] === 'remember-patch-other')
    otherId = e.userId;
});
ircd.on('userPreMessage', e => {
  if (e.details.text.text === 'hello bot') return {action:'pass',patch:{text:'Hello from JavaScript!'}};
  if (e.details.text.text === 'rewrite-body') return {action:'pass',patch:{text:'rewritten: café two  spaces'}};
  if (e.details.text.text === 'patch-excluded') return {action:'pass',patch:{exemptions:[otherId]}};
  if (e.details.text.text === 'patch-denied') return {action:'pass',patch:{text:'must-not-deliver'}};
});
ircd.on('userPreMessage', e => { if (e.details.text.text === 'patch-denied') return {action:'deny'}; });
ircd.on('buildNeighborList', e => {
  if (e.source.id !== otherId) return {action:'pass',patch:{exceptions:[{userId:otherId,include:false}]}};
});
ircd.on('userPart', e => {
  if (e.memb.channel.name.text === '#patches') return {action:'pass',patch:{partmessage:'patched departure'}};
});
""")
    other.barrier("remember-patch-other")
    caller.send("PRIVMSG #patches :hello bot")
    other.until(
        lambda line: " PRIVMSG #patches :" in line and line.endswith(":Hello from JavaScript!")
    )
    caller.send("PRIVMSG #patches :rewrite-body")
    lines = other.until(lambda line: " PRIVMSG #patches :" in line)
    assert lines[-1].endswith(":rewritten: café two  spaces"), lines
    caller.send("PRIVMSG #patches :patch-excluded")
    caller.send("PRIVMSG #patches :patch-denied")
    caller.barrier("patch-deliveries-finished")
    lines = other.barrier("patch-no-delivery")
    assert not any(" PRIVMSG #patches :" in line for line in lines), lines
    caller.send("NICK patchrenamed")
    caller.until(lambda line: " NICK " in line and line.endswith(":patchrenamed"))
    caller.nick = "patchrenamed"
    lines = other.barrier("neighbor-excluded")
    assert not any(" NICK " in line and line.endswith(":patchrenamed") for line in lines), lines
    reload(r"""
ircd.on('buildNeighborList', event => {
  const channels = event.include.items
    .filter(m => m && m.channel && m.channel.name.text === '#public')
    .map(m => m.channel.name.text);
  return {action:'pass',patch:{includeChannels:channels}};
});
ircd.on('userPart', e => ({action:'pass',patch:{partmessage:'patched departure'}}));
""")
    caller.send("NICK patchfiltered")
    caller.until(lambda line: " NICK " in line and line.endswith(":patchfiltered"))
    caller.nick = "patchfiltered"
    lines = other.barrier("neighbor-channels-filtered")
    assert not any(" NICK " in line and line.endswith(":patchfiltered") for line in lines), lines
    caller.send("PART #patches :original departure")
    lines = other.until(lambda line: " PART #patches " in line)
    assert lines[-1].endswith(":patched departure"), lines
    # Invalid wire data poisons the context and cannot partially publish a patch.
    reload(r"""
ircd.on('userPreMessage', e => ({action:'pass',patch:{text:'bad\rline'}}));
""")
    caller.send("PRIVMSG " + other.nick + " :invalid-patch")
    time.sleep(0.3)
    reload("")
    lines = other.barrier("invalid-patch-not-delivered")
    assert not any(" PRIVMSG " in line for line in lines), lines
    reload(GOOD)
    other.send("PART #patches")
    other.barrier("patch-cleanup")
    print(
        "PASS: actual message rewrite, exemptions, deny discard, neighbor exclusions, PART reason and invalid patch rejection"
    )


def exercise_event_listeners(caller, other, reload):
    reload(r"""
const stages = ['accountChange','userPreAway','userPreBack','userAway','userBack',
'capAddDel','capValueChange','namesListItem','stats','whois','whoisLine','whoLine',
'whoMatch','whoVisible','serverLink','serverBurst','serverSplit','syncUser',
'syncChannel','syncNetwork','httpACLCheck','httpRequest'];
const snapshots = new Map();
for (const stage of stages) ircd.on(stage, e => {
  snapshots.set(stage, e);
  if (e.stage !== stage) throw Error('event stage mismatch');
  if (e.userId) ircd.notice(e.userId, 'event:' + stage);
  if (stage === 'userPreAway' && e.message.text === 'blocked-away') return {action:'deny'};
  if (stage === 'userAway' && (!e.away || e.message.text !== 'event-away')) throw Error('away state');
  if (stage === 'userBack' && (e.away || !e.previouslyAway || e.previousMessage.text !== 'event-away')) throw Error('back state');
  if (stage === 'whoisLine' && e.numeric.code === 312) return {action:'deny'};
});
ircd.on('postCommand', e => {
  if (e.command !== 'JSEVENTPROBE') return;
  for (const stage of ['accountChange','capAddDel','capValueChange','serverLink','serverBurst',
      'serverSplit','syncUser','syncChannel','syncNetwork','httpACLCheck','httpRequest']) {
    if (!snapshots.has(stage)) throw Error('missing event:' + stage);
  }
  if (snapshots.get('accountChange').loggedIn || snapshots.get('accountChange').account.text !== '') throw Error('account logout');
  if (snapshots.get('capValueChange').name.text !== 'js-event-probe') throw Error('capability ownership');
  if (snapshots.get('serverLink').serverName.text !== 'event-probe.example') throw Error('server ownership');
  if (snapshots.get('serverSplit').serverId.text !== '999') throw Error('server ID');
  const request = snapshots.get('httpRequest');
  if (request.path.text !== '/js-event-probe' || request.method.text !== 'GET') throw Error('request ownership');
  if ('headers' in request || 'body' in request || 'query' in request) throw Error('HTTP privacy');
  ircd.notice(e.userId, 'event-probe:snapshots-retained');
});
""")
    caller.send("JSEVENTPROBE")
    caller.until(lambda line: line.endswith(":event-probe:snapshots-retained"))
    caller.send("AWAY :blocked-away")
    lines = caller.barrier("away-denied")
    assert not any(" 306 " in line or line.endswith(":event:userAway") for line in lines), lines
    caller.send("AWAY :event-away")
    lines = caller.barrier("away-set")
    assert any(line.endswith(":event:userPreAway") for line in lines), lines
    assert any(line.endswith(":event:userAway") for line in lines), lines
    assert any(" 306 " in line for line in lines), lines
    caller.send("AWAY")
    lines = caller.barrier("away-cleared")
    assert any(line.endswith(":event:userPreBack") for line in lines), lines
    assert any(line.endswith(":event:userBack") for line in lines), lines
    assert any(" 305 " in line for line in lines), lines
    caller.send("WHOIS " + caller.nick)
    lines = caller.until(lambda line: " 318 " in line)
    lines += caller.barrier("event-318-effects")
    assert any(line.endswith(":event:whois") for line in lines), lines
    assert any(line.endswith(":event:whoisLine") for line in lines), lines
    assert any(" 311 " in line for line in lines), lines
    assert not any(" 312 " in line for line in lines), lines
    caller.send("JOIN #events")
    caller.barrier("events-joined")
    caller.send("NAMES #events")
    lines = caller.until(lambda line: " 366 " in line)
    lines += caller.barrier("event-366-effects")
    assert any(line.endswith(":event:namesListItem") for line in lines), lines
    caller.send("WHO #events")
    lines = caller.until(lambda line: " 315 " in line)
    lines += caller.barrier("event-315-effects")
    assert any(line.endswith(":event:whoLine") for line in lines), lines
    caller.send("WHO " + caller.nick)
    lines = caller.until(lambda line: " 315 " in line)
    lines += caller.barrier("event-315-effects")
    assert any(line.endswith(":event:whoMatch") for line in lines), lines
    assert any(line.endswith(":event:whoVisible") for line in lines), lines
    caller.send("STATS u")
    lines = caller.until(lambda line: " 219 " in line)
    lines += caller.barrier("event-219-effects")
    assert any(line.endswith(":event:stats") for line in lines), lines
    reload(GOOD)
    caller.send("PART #events")
    caller.barrier("events-cleanup")
    print(
        "PASS: all 22 listener names registered; real AWAY/BACK, WHOIS filtering, NAMES, WHO and STATS events"
    )


def exercise_webhooks(caller, reload, receiver, directory):
    endpoint = f'<javascriptwebhook name="joins" url="{receiver.url}/joins" loopbackhttp="yes">\n'
    secret = directory / "webhook-secret"
    secret.write_text("synthetic-first-token")
    secret.chmod(0o600)
    header = f'<javascriptwebhookheader endpoint="joins" name="xc-token" valuefile="{secret}">\n'
    endpoint += header
    example = (ROOT / "examples/javascript/join-webhook.js").read_text()
    reload(example, "admin", endpoint)
    before = len(receiver.posts)
    caller.send("JOIN #example")
    caller.barrier("webhook-real-join")
    posts = receiver.wait(before + 1)
    route, body, headers = posts[-1]
    payload = json.loads(body)
    assert route == "/joins" and headers["Content-Type"] == "application/json"
    assert headers["xc-token"] == "synthetic-first-token"
    assert payload == {
        "event": "irc.channel.join",
        "user": {"id": payload["user"]["id"], "nick": caller.nick},
        "channel": "#example",
    }
    assert isinstance(payload["user"]["id"], str) and len(payload["user"]["id"]) == 9
    caller.send("JOIN #private-webhook-test")
    caller.barrier("webhook-private-filter")
    caller.send("PART #example")
    caller.send("JOIN #example")
    caller.barrier("webhook-real-rejoin")
    receiver.wait(before + 2)
    time.sleep(0.2)
    assert len(receiver.posts) == before + 2

    # Initialization attempts fail before reservation or I/O. The entire failed
    # candidate (including a changed target) must leave the last-good set alive.
    changed_endpoint = endpoint.replace("/joins", "/wrong-target")
    reload("ircd.postWebhook('joins', '{}');", "admin", changed_endpoint)
    caller.send("PART #example")
    caller.send("JOIN #example")
    caller.barrier("webhook-failed-rehash-retains-target")
    receiver.wait(before + 3)
    assert receiver.posts[-1][0] == "/joins"

    # Bad/missing files and invalid names fail the complete candidate atomically.
    for bad in (
        header.replace(str(secret), str(secret) + ".missing"),
        header.replace('name="xc-token"', 'name="Host"'),
        header.replace('endpoint="joins"', 'endpoint="missing"'),
        header + header,
    ):
        count = len(receiver.posts)
        reload(example, "admin", endpoint.replace(header, bad).replace("/joins", "/wrong-target"))
        caller.send("PART #example")
        caller.send("JOIN #example")
        caller.barrier("webhook-invalid-header-retains-generation")
        receiver.wait(count + 1)
        assert receiver.posts[-1][0] == "/joins"
        assert receiver.posts[-1][2]["xc-token"] == "synthetic-first-token"
    secret.write_text("synthetic-second-token")
    count = len(receiver.posts)
    caller.send("PART #example")
    caller.send("JOIN #example")
    receiver.wait(count + 1)
    assert receiver.posts[-1][2]["xc-token"] == "synthetic-first-token"
    reload(example, "admin", endpoint)
    count = len(receiver.posts)
    caller.send("PART #example")
    caller.send("JOIN #example")
    receiver.wait(count + 1)
    assert receiver.posts[-1][2]["xc-token"] == "synthetic-second-token"

    # Every failure is tested in a fresh isolate. Denied/error/Promise batches
    # cannot release even their earlier staged requests to the worker.
    for ending in (
        "return {action:'deny'};",
        "throw Error('synthetic');",
        "return Promise.resolve();",
        "while (true) {}",
    ):
        source = (
            "ircd.on('preCommand', e => { if (e.command === 'XWEBTEST') { ircd.postWebhook('joins', '{}'); "
            + ending
            + " } });"
        )
        reload(source, "admin", endpoint)
        count = len(receiver.posts)
        caller.send("XWEBTEST")
        if ending.startswith("return {action"):
            caller.barrier("webhook-denied-batch")
        else:
            assert_silent(caller)
        time.sleep(0.2)
        assert len(receiver.posts) == count

    for source in (
        "ircd.on('preCommand', e => { if (e.command === 'XWEBTEST') ircd.postWebhook('joins', '{'); });",
        "ircd.on('preCommand', e => { if (e.command === 'XWEBTEST') ircd.postWebhook('joins', '[]'); });",
        "ircd.on('preCommand', e => { if (e.command === 'XWEBTEST') ircd.postWebhook('unconfigured', '{}'); });",
        "ircd.on('preCommand', e => { if (e.command === 'XWEBTEST') ircd.postWebhook('joins', JSON.stringify({x:'x'.repeat(16384)})); });",
    ):
        reload(source, "admin", endpoint)
        count = len(receiver.posts)
        caller.send("XWEBTEST")
        assert_silent(caller)
        assert len(receiver.posts) == count

    reload("ircd.on('postJoin', () => ircd.postWebhook('joins', '{}'));", "user", endpoint)
    count = len(receiver.posts)
    caller.send("JOIN #user-webhook-denied")
    caller.barrier("webhook-user-authority")
    time.sleep(0.2)
    assert len(receiver.posts) == count
    reload(
        "ircd.on('preCommand', e => { if(e.command === 'XWEBSTATS') ircd.notice(e.userId, 'webhook-stats:' + JSON.stringify(ircd.webhookStats())); });",
        "admin",
        endpoint,
    )
    caller.send("XWEBSTATS")
    lines = caller.until(lambda line: ":webhook-stats:" in line)
    stats = json.loads(
        next(line.split(":webhook-stats:", 1)[1] for line in lines if ":webhook-stats:" in line)
    )
    assert stats["pending"] == 0 and stats["sent"] >= 3
    assert set(stats) == {"pending", "sent", "failed", "discarded"}
    print(
        "PASS: real JOIN-to-POST/rejoin, privacy filter, failed-rehash target, init/user authority, batch/JSON rejection and aggregate stats"
    )
    reload(GOOD)


def main():
    with tempfile.TemporaryDirectory(prefix="irc-javascript-") as directory:
        path = pathlib.Path(directory)
        script = path / "hooks.js"
        script.write_text(GOOD)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        username = pwd.getpwuid(os.getuid()).pw_name
        base = f'''<server name="js.test" description="Synthetic tests" id="001" network="JSTest">
<admin name="Test" email="test@example.invalid">
<bind address="127.0.0.1" port="{port}" type="clients">
<connect name="test" allow="127.0.0.1" maxlocal="20" maxglobal="20" fakelag="yes" threshold="100000">
<security user="{username}" group="{grp.getgrgid(os.getgid()).gr_name}">
<module name="alias">
<module name="showwhois">
<alias text="INFO" replace="PING :deny">
<alias text="TIME" replace="XLOOP">
<module name="javascript">
<module name="js_hook_probe">
<module name="js_event_probe">
<log method="stdout" type="m_javascript MODULE CONFIG" level="normal">
<javascriptsettings timeoutms="200">
<javascriptwasm name="answer" file="{ROOT / "examples/javascript/answer.wasm"}">
'''
        receiver = Receiver()
        config = path / "inspircd.conf"
        config.write_text(base + f'<javascript file="{script}">\n')
        with (path / "server.log").open("w") as log:
            server = subprocess.Popen(
                [str(RUN / "bin/inspircd"), "--nofork", "--config", str(config)],
                cwd=RUN,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            clients = []
            try:
                time.sleep(0.3)
                if server.poll() is not None:
                    raise AssertionError((path / "server.log").read_text())
                caller = Client("127.0.0.1", port, "jscaller")
                clients.append(caller)
                other = Client("127.0.0.1", port, "jsother")
                clients.append(other)
                other.barrier("ready")
                caller.send("PING :rewrite")
                lines = caller.until(lambda line: line.endswith(":post:SUCCESS"))
                assert any(" 351 jscaller " in line for line in lines), lines
                caller.send("XWASM")
                caller.until(lambda line: " PONG " in line and line.endswith("42"))
                caller.send("XALIAS")
                caller.send("XLOOP")
                caller.send("XSERVER")
                caller.send("XOPERONLY")
                lines = caller.barrier("after-revalidation")
                assert any(" 481 jscaller " in line for line in lines), lines
                assert not any(
                    line.endswith(":deny")
                    or "forbidden-server-effect" in line
                    or "did a /WHOIS on you" in line
                    for line in lines
                ), lines
                caller.send("OPER missing synthetic-password")
                caller.until(lambda line: line.endswith(":oper-post:FAILURE"))
                caller.sock.sendall(b"XBYTES :\xff\r\n")
                until_raw(caller, lambda line: b" PONG " in line and line.endswith(b":\xff"))
                caller.send("PING :deny")
                lines = caller.barrier("after-deny")
                assert not any(
                    line.endswith(":deny") or "must-discard" in line for line in lines
                ), lines
                caller.send("XRENAME")  # Denied callbacks discard queued mutations.
                caller.barrier("after-discard")
                assert irc_nick(caller, "jscaller")
                caller.send("XSELF")
                caller.until(lambda line: " NICK " in line and line.endswith(":selfrenamed"))
                caller.nick = "selfrenamed"
                caller.send("XNOTICE")
                caller.until(lambda line: line.endswith(":owned: café two  spaces"))

                def reload(source, authority="user", webhook_config=""):
                    script.write_text(source)
                    if os.environ.get("JS_NATIVE_TRACE"):
                        print("RELOAD", authority, source[:250], flush=True)
                    config.write_text(
                        base
                        + webhook_config
                        + f'<javascript file="{script}" authority="{authority}">\n'
                    )
                    offset = (path / "server.log").stat().st_size
                    server.send_signal(signal.SIGHUP)
                    deadline = time.monotonic() + 15
                    while time.monotonic() < deadline:
                        output = (path / "server.log").read_text()[offset:]
                        if (
                            "JavaScript configuration loaded" in output
                            or "Unable to read the configuration for m_javascript" in output
                        ):
                            break
                        assert server.poll() is None
                        time.sleep(0.1)
                    else:
                        raise AssertionError("Rehash did not complete")

                exercise_validated_commands(caller, reload)
                exercise_module_hooks(caller, reload)
                output = (path / "server.log").read_text()
                assert "script=1 stage=userPreNick category=exception" in output
                assert "script=1 stage=userPreMessage category=timeout" in output
                exercise_native_mutations(caller, other, reload)
                exercise_event_listeners(caller, other, reload)
                exercise_webhooks(caller, reload, receiver, path)
                exercise_messaging(caller, other, reload)
                output = (path / "server.log").read_text()
                for marker in ("synthetic-first-token", "synthetic-second-token", "webhook-secret"):
                    assert marker not in output

                exercise_wchat_nocodb(caller, other, reload, receiver, path)
                output = (path / "server.log").read_text()
                for marker in ("synthetic-adapter-token", "adapter-secret"):
                    assert marker not in output
                reload(GOOD)
                reload('throw Error("private-init-sentinel");')
                output = (path / "server.log").read_text()
                assert "script=1 stage=initialization category=exception" in output
                assert "private-init-sentinel" not in output
                caller.send("XWASM")  # Last-good context survived failed reload.
                caller.until(lambda line: " PONG " in line and line.endswith("42"))
                reload(GOOD, "admin")
                other.barrier("remember-other")
                caller.send("XADMIN")
                other.until(lambda line: " NICK " in line and line.endswith(":adminrenamed"))
                other.nick = "adminrenamed"
                reload(GOOD)
                other.barrier("remember-user-target")
                caller.send("XSPOOF")  # Known other-user UUID is rejected in user authority.
                caller.send("PING :must-not-run")
                assert_silent(caller)
                reload(GOOD)
                caller.barrier("recovered")
                reload("ircd.on('preCommand', e => ({action:'pass'}));")
                caller.send("FOO-BAR " + " ".join(["x"] * 20))
                caller.barrier("unusual-input-still-healthy")
                reload(
                    "ircd.on('preCommand', e => { if (e.command === 'PING') return {action:'pass',params:['bad\\rline']}; });"
                )
                caller.send("PING :invalid-rewrite")
                assert_silent(caller)
                reload("ircd.on('preCommand', e => { if (e.command === 'PING') while(true) {} });")
                caller.send("PING :timeout")
                assert_silent(caller)
                assert server.poll() is None
                assert "script=1 stage=preCommand category=timeout" in (path / "server.log").read_text()
                reload("ircd.on('postCommand', e => { throw Error('post failed'); });")
                caller.barrier("post-error-cannot-undo")
                caller.barrier("post-only-does-not-lock-pre")
                assert "script=1 stage=postCommand category=exception" in (path / "server.log").read_text()
                script.write_text(GOOD)
                config.write_text(
                    base.replace('<module name="javascript">', "")
                    + f'<javascript file="{script}">\n'
                )
                offset = (path / "server.log").stat().st_size
                server.send_signal(signal.SIGHUP)
                deadline = time.monotonic() + 15
                while (
                    "The m_javascript.so module was unloaded"
                    not in (path / "server.log").read_text()[offset:]
                ):
                    assert time.monotonic() < deadline, "Module unload did not complete"
                    assert server.poll() is None
                    time.sleep(0.1)
                caller.send("XWASM")
                lines = caller.barrier("after-module-unload")
                assert any(" XWASM :Unknown command" in line for line in lines), lines
                reload(GOOD)
                caller.send("XWASM")
                caller.until(lambda line: " PONG " in line and line.endswith("42"))
                print(
                    "PASS: native rewrite, Wasm, authority, failclosed/reload, and actual module unload/reload"
                )
            except Exception:
                print((path / "server.log").read_text())
                raise
            finally:
                for client in clients:
                    client.close()
                server.terminate()
                server.wait(timeout=15)
                receiver.close()


def until_raw(client, predicate):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if b"\n" not in client.buffer:
            client.sock.settimeout(max(0.01, deadline - time.monotonic()))
            data = client.sock.recv(4096)
            assert data
            client.buffer += data
            continue
        line, client.buffer = client.buffer.split(b"\n", 1)
        if predicate(line.rstrip(b"\r")):
            return
    raise AssertionError("Raw response absent")


def irc_nick(client, expected):
    client.send("WHOIS " + expected)
    return any(
        " 311 " in line and (" " + expected + " ") in line
        for line in client.until(lambda line: " 318 " in line)
    )


def assert_silent(client):
    # Consume any earlier UNKNOWN replies, then verify the blocked PING produced no PONG.
    client.sock.settimeout(0.7)
    data = client.buffer
    client.buffer = b""
    try:
        while True:
            part = client.sock.recv(4096)
            if not part:
                raise AssertionError("Server disconnected client")
            data += part
    except TimeoutError:
        pass
    assert b" PONG " not in data, data


if __name__ == "__main__":
    main()
    exercise_remote_messaging()
