"""Real InspIRCd v4 + real companion, fake SSH; all sockets stay in --network none."""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time

BASE = '''<server name="test.example" description="test" network="test" id="001">
<admin name="test" email="test@example.invalid" description="test">
<bind address="127.0.0.1" port="16669" type="clients">
<connect allow="*" timeout="20" useident="no" resolvehostnames="no" fakelag="no">
<files motd="/tmp/motd">
<path moduledir="/inspircd/modules">
<power diepass="unused" restartpass="unused">
<module name="reverse_tunnel">
<class name="admin" commands="*" privs="servers/reverse-tunnel">
<class name="limited" commands="*">
<type name="admin" classes="admin">
<type name="limited" classes="limited">
<oper name="admin" password="testpass" host="*@127.0.0.1" type="admin">
<oper name="limited" password="testpass" host="*@127.0.0.1" type="limited">
<tunnelprofile name="edge" host="gateway.example" user="tunnel" identityfile="/synthetic/key"
 knownhostsfile="/synthetic/hosts" destination="127.0.0.1" listenmin="16670" listenmax="16680" destmin="6667" destmax="6697">
'''
DECL = '<reversetunnel name="decl" profile="edge" listenport="16670" destinationport="6667">\n'
CONFIG = Path('/tmp/tunnel.conf')

def eventually(fn, timeout=25):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        result = fn()
        if result:
            return result
        time.sleep(.1)
    raise AssertionError('condition timed out')

def events():
    try:
        return [json.loads(line) for line in Path('/tmp/ssh-events').read_text().splitlines()]
    except FileNotFoundError:
        return []

def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False

class IRC:
    def __init__(self, nick):
        self.sock = socket.create_connection(('127.0.0.1', 16669))
        self.sock.settimeout(.2)
        self.send(f'NICK {nick}\r\nUSER test 0 * :test')
        self.until(' 001 ')
    def send(self, text):
        self.sock.sendall((text + '\r\n').encode())
    def until(self, expected, timeout=25):
        result = ''
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                data = self.sock.recv(65536)
                assert data, result
                result += data.decode()
            except socket.timeout:
                continue
            if expected in result:
                return result
        raise AssertionError(f'missing {expected!r} in {result!r}')
    def command(self, text, expected):
        self.send(text)
        return self.until(expected)

Path('/tmp/motd').write_text('test\n')
CONFIG.write_text(BASE + DECL)
log = open('/tmp/daemon.log', 'w')
daemon = subprocess.Popen(['/inspircd/bin/inspircd', '--nofork', '--config', str(CONFIG)], stdout=log, stderr=log, cwd='/inspircd')
try:
    def ready():
        assert daemon.poll() is None, Path('/tmp/daemon.log').read_text()
        try:
            with socket.create_connection(('127.0.0.1', 16669), timeout=.1):
                return True
        except OSError:
            return False
    eventually(ready)
    user = IRC('tester')
    user.command('RTUNNEL LIST', ' 481 ')
    user.command('OPER limited testpass', ' 381 ')
    user.command('RTUNNEL LIST', 'requires a local operator')
    user.command('OPER admin testpass', ' 381 ')
    first = eventually(lambda: events() and events()[0])
    assert first['argv'][-1] == 'gateway.example'
    assert '-oStrictHostKeyChecking=yes' in first['argv']
    assert '127.0.0.1:16670:127.0.0.1:6667' in first['argv']
    status = user.command('RTUNNEL SHOW decl', 'owner=config')
    assert '/synthetic/' not in status and 'gateway.example' not in status
    user.command('RTUNNEL DELETE decl', 'config-owned tunnel')
    user.command('RTUNNEL ADD bad edge 22 6667', 'outside profile bounds')
    user.command('RTUNNEL ADD duplicate edge 16670 6667', 'duplicate remote listener')
    user.command('RTUNNEL ADD dyn edge 16671 6667', 'change accepted')
    eventually(lambda: len(events()) == 2)
    dynpid = events()[-1]['pid']
    user.command('RTUNNEL UPDATE dyn edge 16672 6667', 'change accepted')
    eventually(lambda: not alive(dynpid))
    eventually(lambda: len(events()) == 3)
    dynpid = events()[-1]['pid']
    user.command('REHASH', 'Rehashing')
    time.sleep(2)
    user.command('RTUNNEL SHOW dyn', 'owner=dynamic')
    assert alive(first['pid']) and alive(dynpid) and len(events()) == 3
    # Bad candidate must not destroy either existing tunnel.
    CONFIG.write_text(BASE + DECL + DECL)
    user.command('REHASH', 'Rehashing')
    user.until('duplicate tunnel name')
    assert alive(first['pid']) and alive(dynpid)
    # Config removes only declarative entries; dynamic entries survive.
    CONFIG.write_text(BASE)
    user.command('REHASH', 'Rehashing')
    eventually(lambda: not alive(first['pid']))
    assert alive(dynpid)
    user.command('RTUNNEL SHOW decl', 'unknown tunnel')
    # Child exit is detected and restarted, with increasing backoff.
    Path('/tmp/ssh-fail').touch()
    os.kill(dynpid, signal.SIGKILL)
    eventually(lambda: len(events()) >= 6, timeout=60)
    recent = events()[-3:]
    assert recent[1]['time'] - recent[0]['time'] >= .9
    assert recent[2]['time'] - recent[1]['time'] >= 1.9
    Path('/tmp/ssh-fail').unlink()
    eventually(lambda: len(events()) >= 7, timeout=60)
    current = events()[-1]['pid']
    user.command('RTUNNEL DELETE dyn', 'change accepted')
    eventually(lambda: not alive(current))
    # Disabled desired state is retained but never launches a process.
    user.command('RTUNNEL ADD paused edge 16673 6667 disabled', 'change accepted')
    time.sleep(12)
    assert len(events()) == 7
    user.command('RTUNNEL SHOW paused', 'state=stopped')
    user.command('RTUNNEL ENABLE paused', 'change accepted')
    eventually(lambda: len(events()) == 8)
    pausedpid = events()[-1]['pid']
    user.command('RTUNNEL DISABLE paused', 'change accepted')
    eventually(lambda: not alive(pausedpid))
    # UPDATE without state preserves disabled; rehash also preserves it.
    user.command('RTUNNEL UPDATE paused edge 16674 6667', 'change accepted')
    user.command('REHASH', 'Rehashing')
    time.sleep(12)
    assert len(events()) == 8
    user.command('RTUNNEL SHOW paused', 'state=stopped')
    user.command('RTUNNEL DELETE paused', 'change accepted')
    time.sleep(12)
    assert len(events()) == 8
    user.command('RTUNNEL ADD unload edge 16673 6667', 'change accepted')
    eventually(lambda: len(events()) >= 9)
    current = events()[-1]['pid']
    user.command('UNLOADMODULE reverse_tunnel', 'successfully unloaded')
    eventually(lambda: not alive(current))
    user.command('LOADMODULE reverse_tunnel', 'successfully loaded')
    status = user.command('RTUNNEL LIST', 'end of list')
    assert 'owner=' not in status
    user.command('RTUNNEL ADD crash edge 16674 6667', 'change accepted')
    eventually(lambda: len(events()) >= 10)
    current = events()[-1]['pid']
    daemon.kill()
    daemon.wait(timeout=5)
    eventually(lambda: not alive(current))
    print('PASS: real module CRUD, privileges, argv, atomic rehash, ownership, backoff, disable/delete no resurrection, unload, restart volatility, daemon crash cleanup')
finally:
    if daemon.poll() is None:
        daemon.terminate()
        daemon.wait(timeout=10)
    log.close()
    print(Path('/tmp/daemon.log').read_text()[-4000:])
