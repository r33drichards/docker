#!/usr/bin/env python3
"""Synthetic loopback integration. Requires a built native InspIRCd installation.
Run: INSPIRCD_NATIVE=/path/to/run tests/javascript.sh
The installation must already contain m_javascript.so. No existing config is read.
"""
import os
import grp
import pathlib
import pwd
import signal
import socket
import subprocess
import tempfile
import time
from precommand_client import Client

ROOT = pathlib.Path(__file__).resolve().parent.parent
RUN = pathlib.Path(os.environ['INSPIRCD_NATIVE']).resolve()
GOOD = r'''
if (!ircd.match('hello', 'h*') || !ircd.matchCIDR('192.0.2.4', '192.0.2.0/24') ||
    !ircd.isNick('ValidNick') || !ircd.isChannel('#valid') ||
    ircd.stripColor('\x0304red') !== 'red' || ircd.time() <= 0) throw Error('native utilities');
const wasm = new WebAssembly.Instance(new WebAssembly.Module(ircd.wasm('answer')), {});
const users = new Map();
ircd.on('preCommand', e => {
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
'''


def main():
    with tempfile.TemporaryDirectory(prefix='irc-javascript-') as directory:
        path = pathlib.Path(directory)
        script = path / 'hooks.js'
        script.write_text(GOOD)
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
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
<log method="stdout" type="m_javascript MODULE CONFIG" level="normal">
<javascriptsettings timeoutms="200">
<javascriptwasm name="answer" file="{ROOT / 'examples/javascript/answer.wasm'}">
'''
        config = path / 'inspircd.conf'
        config.write_text(base + f'<javascript file="{script}">\n')
        with (path / 'server.log').open('w') as log:
            server = subprocess.Popen([str(RUN / 'bin/inspircd'), '--nofork', '--config', str(config)], cwd=RUN, stdout=log, stderr=subprocess.STDOUT)
            clients = []
            try:
                time.sleep(.3)
                if server.poll() is not None:
                    raise AssertionError((path / 'server.log').read_text())
                caller = Client('127.0.0.1', port, 'jscaller'); clients.append(caller)
                other = Client('127.0.0.1', port, 'jsother'); clients.append(other)
                other.barrier('ready')
                caller.send('PING :rewrite')
                lines = caller.until(lambda line: line.endswith(':post:SUCCESS'))
                assert any(' 351 jscaller ' in line for line in lines), lines
                caller.send('XWASM')
                caller.until(lambda line: ' PONG ' in line and line.endswith('42'))
                caller.send('XALIAS')
                caller.send('XLOOP')
                caller.send('XSERVER')
                caller.send('XOPERONLY')
                lines = caller.barrier('after-revalidation')
                assert any(' 481 jscaller ' in line for line in lines), lines
                assert not any(line.endswith(':deny') or 'forbidden-server-effect' in line or 'did a /WHOIS on you' in line for line in lines), lines
                caller.send('OPER missing synthetic-password')
                caller.until(lambda line: line.endswith(':oper-post:FAILURE'))
                caller.sock.sendall(b'XBYTES :\xff\r\n')
                until_raw(caller, lambda line: b' PONG ' in line and line.endswith(b':\xff'))
                caller.send('PING :deny')
                lines = caller.barrier('after-deny')
                assert not any(line.endswith(':deny') or 'must-discard' in line for line in lines), lines
                caller.send('XRENAME')  # Denied callbacks discard queued mutations.
                caller.barrier('after-discard')
                assert irc_nick(caller, 'jscaller')
                caller.send('XSELF')
                caller.until(lambda line: ' NICK ' in line and line.endswith(':selfrenamed'))
                caller.nick = 'selfrenamed'
                caller.send('XNOTICE')
                caller.until(lambda line: line.endswith(':owned: café two  spaces'))

                def reload(source, authority='user'):
                    script.write_text(source)
                    config.write_text(base + f'<javascript file="{script}" authority="{authority}">\n')
                    offset = (path / 'server.log').stat().st_size
                    server.send_signal(signal.SIGHUP)
                    deadline = time.monotonic() + 15
                    while time.monotonic() < deadline:
                        output = (path / 'server.log').read_text()[offset:]
                        if 'JavaScript configuration loaded' in output or 'Unable to read the configuration for m_javascript' in output:
                            break
                        assert server.poll() is None
                        time.sleep(.1)
                    else: raise AssertionError('Rehash did not complete')

                reload('throw Error("candidate fails");')
                caller.send('XWASM')  # Last-good context survived failed reload.
                caller.until(lambda line: ' PONG ' in line and line.endswith('42'))
                reload(GOOD, 'admin')
                other.barrier('remember-other')
                caller.send('XADMIN')
                other.until(lambda line: ' NICK ' in line and line.endswith(':adminrenamed'))
                other.nick = 'adminrenamed'
                reload(GOOD)
                other.barrier('remember-user-target')
                caller.send('XSPOOF')  # Known other-user UUID is rejected in user authority.
                caller.send('PING :must-not-run')
                assert_silent(caller)
                reload(GOOD)
                caller.barrier('recovered')
                reload("ircd.on('preCommand', e => ({action:'pass'}));")
                caller.send('FOO-BAR ' + ' '.join(['x'] * 20))
                caller.barrier('unusual-input-still-healthy')
                reload("ircd.on('preCommand', e => { if (e.command === 'PING') return {action:'pass',params:['bad\\rline']}; });")
                caller.send('PING :invalid-rewrite')
                assert_silent(caller)
                reload("ircd.on('preCommand', e => { if (e.command === 'PING') while(true) {} });")
                caller.send('PING :timeout')
                assert_silent(caller)
                assert server.poll() is None
                reload("ircd.on('postCommand', e => { throw Error('post failed'); });")
                caller.barrier('post-error-cannot-undo')
                caller.barrier('post-only-does-not-lock-pre')
                script.write_text(GOOD)
                config.write_text(base.replace('<module name="javascript">', '') + f'<javascript file="{script}">\n')
                offset = (path / 'server.log').stat().st_size
                server.send_signal(signal.SIGHUP)
                deadline = time.monotonic() + 15
                while 'The m_javascript.so module was unloaded' not in (path / 'server.log').read_text()[offset:]:
                    assert time.monotonic() < deadline, 'Module unload did not complete'
                    assert server.poll() is None
                    time.sleep(.1)
                caller.send('XWASM')
                lines = caller.barrier('after-module-unload')
                assert any(' XWASM :Unknown command' in line for line in lines), lines
                reload(GOOD)
                caller.send('XWASM')
                caller.until(lambda line: ' PONG ' in line and line.endswith('42'))
                print('PASS: native rewrite, Wasm, authority, failclosed/reload, and actual module unload/reload')
            except Exception:
                print((path / 'server.log').read_text())
                raise
            finally:
                for client in clients: client.close()
                server.terminate()
                server.wait(timeout=15)


def until_raw(client, predicate):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if b'\n' not in client.buffer:
            client.sock.settimeout(max(.01, deadline - time.monotonic()))
            data = client.sock.recv(4096)
            assert data
            client.buffer += data
            continue
        line, client.buffer = client.buffer.split(b'\n', 1)
        if predicate(line.rstrip(b'\r')): return
    raise AssertionError('Raw response absent')


def irc_nick(client, expected):
    client.send('WHOIS ' + expected)
    return any(' 311 ' in line and (' ' + expected + ' ') in line for line in client.until(lambda line: ' 318 ' in line))


def assert_silent(client):
    # Consume any earlier UNKNOWN replies, then verify the blocked PING produced no PONG.
    client.sock.settimeout(.7)
    data = client.buffer
    client.buffer = b''
    try:
        while True:
            part = client.sock.recv(4096)
            if not part: raise AssertionError('Server disconnected client')
            data += part
    except socket.timeout:
        pass
    assert b' PONG ' not in data, data

if __name__ == '__main__': main()
