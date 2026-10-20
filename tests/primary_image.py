#!/usr/bin/env python3
"""Smoke-test primary-image packaging with disposable config and local synthetic traffic."""

import subprocess
import time
import uuid

from precommand_client import Client

SQLITE_SCRIPT = """
let heap = 0;
ircd.on('preCommand', e => {
  if (e.validated || e.command !== 'XSQLIMAGE') return;
  const operation = e.params[0];
  let result;
  try {
    if (operation === 'ready') {
      result = generation;
    } else if (operation === 'ungranted') {
      let denied = false;
      try { ircd.sqlite.open('ci'); } catch (_) { denied = true; }
      if (!denied) throw Error('implicit database grant');
      result = 'ok';
    } else {
      const db = ircd.sqlite.open('ci');
      if (operation === 'write') {
        db.exec('CREATE TABLE state (value TEXT NOT NULL)');
        db.exec('INSERT INTO state(value) VALUES(?)', [e.params[1]]);
        heap = 1;
      } else if (operation !== 'read') {
        throw Error('unknown operation');
      }
      result = db.query('SELECT value FROM state').rows[0][0] + ':' + heap;
      db.close();
    }
  } catch (error) {
    result = 'FAIL:' + String(error.message);
  }
  return {action:'pass', command:'PING',
          params:['sqlite-image:' + operation + ':' + result]};
});
"""

PERSISTENCE_OPER = """
<class name="PackagingPersistence" commands="MODE" chanmodes="P">
<type name="PackagingPersistence" classes="PackagingPersistence">
<oper name="packaging" password="fake-primary-image-oper-password" host="*@*"
      sslonly="no" type="PackagingPersistence">
"""


def docker(*args):
    return subprocess.check_output(["docker", *args], text=True, timeout=120)


def check_irc(container):
    port = int(docker("port", container, "6667/tcp").strip().rsplit(":", 1)[1])
    caller = Client("127.0.0.1", port, "packagingcaller")
    observer = None
    try:
        observer = Client("127.0.0.1", port, "packagingobserver")
        caller.barrier("primary-image-ping")
        caller.send("JOIN #packaging")
        caller.barrier("caller-joined")
        observer.send("JOIN #packaging")
        observer.until(lambda line: " JOIN " in line and "#packaging" in line)
        caller.send("PRIVMSG #packaging :primary-image-message")
        observer.until(lambda line: line.endswith("PRIVMSG #packaging :primary-image-message"))
    finally:
        caller.close()
        if observer is not None:
            observer.close()


def check_nocodb_token(container):
    docker(
        "exec",
        "--user",
        "10000:10000",
        container,
        "sh",
        "-ec",
        'test "$(stat -c %u:%g:%a /inspircd/.secrets)" = 10000:10000:700; '
        'test "$(stat -c %u:%g:%a /inspircd/.secrets/nocodb-token)" = 10000:10000:600; '
        'test "$(cat /inspircd/.secrets/nocodb-token)" = fake-primary-image-token',
    )
    inherited = docker("exec", "--user", "10000:10000", container, "cat", "/proc/1/environ")
    assert "INSPIRCD_NOCODB_TOKEN=" not in inherited
    assert "fake-primary-image-token" not in inherited
    assert "fake-primary-image-token" not in docker("logs", container)


def check_nocodb_activation(image):
    # Exercise the shipped entrypoint/config/script path with a fake token.
    # check_irc only uses #packaging, so no event targets the real NocoDB service.
    container = docker(
        "run",
        "-d",
        "-p",
        "127.0.0.1::6667",
        "-e",
        "INSPIRCD_NOCODB_TOKEN=fake-primary-image-token",
        "-e",
        "INSPIRCD_NOCODB_WCHAT=yes",
        image,
    ).strip()
    try:
        check_irc(container)
        maps = docker("exec", container, "cat", "/proc/1/maps")
        assert "m_javascript.so" in maps
        assert "m_precommand.so" not in maps
        check_nocodb_token(container)
        print(
            "PASS: shipped NocoDB opt-in startup, secret permissions, environment removal, ordinary IRC"
        )
    finally:
        docker("rm", "-f", container)


def configure_sqlite(container, caller, generation, *, grant=False, first=False):
    # Files and rehash are deployment-owned; the bootstrap must drop root before
    # the real entrypoint creates the config, token, certificates, or daemon.
    docker(
        "exec",
        "--user",
        "10000:10000",
        container,
        "sh",
        "-ec",
        'printf "%s\\n" "$1" > /inspircd/conf/packaging-sqlite.js; '
        'printf "%s\\n" "$2" > /inspircd/conf/packaging-sqlite.conf; '
        'if [ "$3" = yes ]; then '
        "printf '\\n<include file=\"packaging-sqlite.conf\">\\n' "
        ">> /inspircd/conf/inspircd.conf; fi",
        "sh",
        f"const generation = '{generation}';\n" + SQLITE_SCRIPT,
        '<javascript file="packaging-sqlite.js"'
        + (' databases="ci">' if grant else ">")
        + PERSISTENCE_OPER,
        "yes" if first else "no",
    )
    docker("kill", "--signal", "HUP", container)
    # Poll a side-effect-free hook rather than racing asynchronous rehash with a
    # fixed sleep. Rewriting to PING gives an immediate, ordered IRC response.
    deadline = time.monotonic() + 30
    expected = "sqlite-image:ready:" + generation
    while time.monotonic() < deadline:
        caller.send("XSQLIMAGE ready")
        lines = caller.barrier("sqlite-image-rehash")
        if any(" PONG " in line and line.endswith(expected) for line in lines):
            return
        time.sleep(0.2)
    raise AssertionError("SQLite fixture rehash did not finish:\n" + docker("logs", container))


def sqlite_case(caller, operation, expected, argument=""):
    caller.send("XSQLIMAGE " + operation + (" " + argument if argument else ""))
    marker = "sqlite-image:" + operation + ":"
    lines = caller.until(lambda line: " PONG " in line and marker in line)
    assert lines[-1].endswith(marker + expected), lines


def persistent_checksums(container):
    # Compare hashes only: never copy or print the generated private key.
    return docker(
        "exec",
        "--user",
        "10000:10000",
        container,
        "sha256sum",
        "/inspircd/conf/inspircd.conf",
        "/inspircd/conf/packaging-sqlite.conf",
        "/inspircd/conf/packaging-sqlite.js",
        "/inspircd/conf/cert.pem",
        "/inspircd/conf/key.pem",
        "/inspircd/conf/dhparams.pem",
    )


def channel_modes(caller, channel):
    caller.send("MODE " + channel)
    lines = caller.until(lambda line: f" 324 {caller.nick} {channel} " in line)
    return lines[-1].split()[4]


def make_permanent_channel(container, caller, marker):
    channel = "#packaging-persistent"
    caller.send("JOIN " + channel)
    caller.until(lambda line: " JOIN " in line and channel in line)
    assert "P" not in channel_modes(caller, channel), "Channels must not be permanent implicitly"
    caller.send("OPER packaging fake-primary-image-oper-password")
    caller.until(lambda line: f" 381 {caller.nick} " in line)
    caller.send("MODE " + channel + " +P")
    caller.until(
        lambda line: " MODE " + channel + " " in line and line.split()[-1].lstrip(":") == "+P"
    )
    caller.send("TOPIC " + channel + " :persistent-topic-" + marker)
    caller.until(lambda line: line.endswith(" TOPIC " + channel + " :persistent-topic-" + marker))
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        saved = docker(
            "exec",
            "--user",
            "10000:10000",
            container,
            "sh",
            "-ec",
            "if [ -f /inspircd/data/permchannels.conf ]; "
            "then cat /inspircd/data/permchannels.conf; fi",
        )
        if channel in saved and "persistent-topic-" + marker in saved:
            return
        time.sleep(0.5)
    raise AssertionError("Synthetic permanent channel was not saved")


def fill_disposable_logs(image, volume):
    # The daemon is stopped. Seed exactly five synthetic, sparse 10 MiB files
    # so its next real startup must rotate without needing an IRC log flood.
    docker(
        "run",
        "--rm",
        "--user",
        "10000:10000",
        "--entrypoint",
        "sh",
        "--mount",
        f"type=volume,src={volume},dst=/inspircd/persistent,volume-nocopy",
        image,
        "-ec",
        "umask 077; "
        "for suffix in active 1 2 3 4; do "
        "file=/inspircd/persistent/logs/inspircd.log; "
        '[ "$suffix" = active ] || file="$file.$suffix"; '
        'printf "packaging-log-%s\\n" "$suffix" > "$file"; '
        'truncate -s 10485760 "$file"; done',
    )


def check_persistent_volume(image):
    volume = "primary-image-persistent-" + uuid.uuid4().hex
    marker = uuid.uuid4().hex
    container = None
    caller = None
    checksums = None
    docker("volume", "create", volume)
    try:
        for replacement in (False, True):
            container = docker(
                "run",
                "-d",
                "--user",
                "0",
                "--group-add",
                "12345",
                "-p",
                "127.0.0.1::6667",
                "--mount",
                f"type=volume,src={volume},dst=/inspircd/persistent,volume-nocopy",
                "-e",
                "INSPIRCD_PERSISTENT=yes",
                "-e",
                "INSPIRCD_SQLITE=yes",
                "-e",
                "INSPIRCD_PERSISTENT_IMPORT=" + ("yes" if replacement else "no"),
                "-e",
                "INSPIRCD_NOCODB_TOKEN=fake-primary-image-token",
                "-e",
                "INSPIRCD_NOCODB_WCHAT=yes",
                image,
            ).strip()
            if replacement:
                deadline = time.monotonic() + 30
                while "waiting for approved TLS import" not in subprocess.check_output(
                    ["docker", "logs", container], text=True, stderr=subprocess.STDOUT, timeout=10
                ):
                    if time.monotonic() >= deadline:
                        raise AssertionError("replacement did not enter the import gate")
                    time.sleep(0.1)
                assert persistent_checksums(container) == checksums
                # Exercise the real image gate before any IRC listener starts.
                # All credentials here are disposable synthetic test material.
                docker(
                    "exec",
                    "--user",
                    "10000:10000",
                    container,
                    "sh",
                    "-ec",
                    "if nc -z 127.0.0.1 6667; then exit 1; fi; "
                    "cd /inspircd/persistent/conf; umask 077; "
                    "test ! -e .migration-ready; "
                    "{ printf 'persistent-import-v1\\n'; "
                    "sha256sum key.pem cert.pem | awk '{print $1 \" \" $2}'; "
                    "} > .migration-ready.new; mv .migration-ready.new .migration-ready",
                )
            check_irc(container)  # Never joins #wchat or triggers the real webhook.
            status = dict(
                line.split(":", 1)
                for line in docker(
                    "exec", "--user", "10000:10000", container, "cat", "/proc/1/status"
                ).splitlines()
            )
            for field in ("Uid", "Gid"):
                assert status[field].split() == ["10000"] * 4, status
            assert not status["Groups"].split(), status
            maps = docker("exec", "--user", "10000:10000", container, "cat", "/proc/1/maps")
            for module in ("m_javascript.so", "m_permchannels.so", "m_log_bounded.so"):
                assert module in maps, module + " must be loaded"
            assert "m_precommand.so" not in maps
            docker(
                "exec",
                "--user",
                "10000:10000",
                container,
                "sh",
                "-ec",
                'test "$(stat -c %u:%g:%a /inspircd/persistent)" = 0:0:755; '
                "for child in conf data logs; do "
                'test "$(readlink "/inspircd/$child")" = "/inspircd/persistent/$child"; '
                'test "$(stat -c %u:%g:%a "/inspircd/persistent/$child")" = 10000:10000:700; '
                "done; "
                'test "$(stat -c %u:%g:%a /inspircd/data/script-databases)" = 10000:10000:700; '
                "test -s /inspircd/logs/inspircd.log; "
                'sh /inspircd/conf/logging.sh | grep -q \'maxbytes="10485760" files="5"\'',
            )
            check_nocodb_token(container)
            port = int(docker("port", container, "6667/tcp").strip().rsplit(":", 1)[1])
            caller = Client("127.0.0.1", port, "sqlitepackaging")
            if not replacement:
                configure_sqlite(container, caller, "ungranted", first=True)
                sqlite_case(caller, "ungranted", "ok")
                configure_sqlite(container, caller, "granted", grant=True)
                sqlite_case(caller, "write", marker + ":1", marker)
                sqlite_case(caller, "read", marker + ":1")
                make_permanent_channel(container, caller, marker)
                checksums = persistent_checksums(container)
            else:
                # No config rewrite or rehash: the saved deployment edits must
                # load on startup, while process-local JavaScript state resets.
                assert persistent_checksums(container) == checksums, (
                    "Config/TLS changed on replacement"
                )
                sqlite_case(caller, "ready", "granted")
                sqlite_case(caller, "read", marker + ":0")
                assert "P" in channel_modes(caller, "#packaging-persistent")
                caller.send("TOPIC #packaging-persistent")
                lines = caller.until(
                    lambda line, nick=caller.nick: f" 332 {nick} #packaging-persistent " in line
                )
                assert lines[-1].endswith(":persistent-topic-" + marker), lines
                docker(
                    "exec",
                    "--user",
                    "10000:10000",
                    container,
                    "sh",
                    "-ec",
                    'test "$(head -n 1 /inspircd/logs/inspircd.log.1)" = packaging-log-active; '
                    'test "$(head -n 1 /inspircd/logs/inspircd.log.4)" = packaging-log-3; '
                    "for suffix in active 1 2 3 4; do "
                    "file=/inspircd/logs/inspircd.log; "
                    '[ "$suffix" = active ] || file="$file.$suffix"; '
                    'test "$(stat -c %u:%g:%a "$file")" = 10000:10000:600; '
                    'test "$(stat -c %s "$file")" -le 10485760; done; '
                    "test ! -e /inspircd/logs/inspircd.log.5",
                )
            docker(
                "exec",
                "--user",
                "10000:10000",
                container,
                "sh",
                "-ec",
                'test "$(stat -c %u:%g:%a /inspircd/data/script-databases/ci.sqlite)" '
                "= 10000:10000:600",
            )
            caller.close()
            caller = None
            docker("stop", container)
            docker("rm", container)
            container = None
            if not replacement:
                fill_disposable_logs(image, volume)
        print(
            "PASS: private persistent layout, dropped privileges, token/config/TLS safety, "
            "SQLite grants/durability, reset JS heap, permanent channel restoration, bounded log rotation"
        )
    except Exception:
        if container is not None:
            print(docker("logs", container))
        raise
    finally:
        if caller is not None:
            caller.close()
        if container is not None:
            docker("rm", "-f", container)
        docker("volume", "rm", volume)


def main():
    image = "inspircd:testing"
    volume = "primary-image-" + uuid.uuid4().hex
    container = None
    docker("volume", "create", volume)
    try:
        docker(
            "run",
            "--rm",
            "--entrypoint",
            "sh",
            image,
            "-ec",
            'test "$(id -u):$(id -g)" = 10000:10000; '
            "test ! -e /inspircd/data/script-databases; "
            "test -s /inspircd/modules/m_javascript.so; "
            "test ! -e /inspircd/modules/m_js_hook_probe.so; "
            "ldd /usr/lib/libnode.so.127",
        )
        container = docker(
            "run",
            "-d",
            "-p",
            "127.0.0.1::6667",
            "-v",
            volume + ":/inspircd/conf",
            image,
        ).strip()
        check_irc(container)
        maps = docker("exec", container, "cat", "/proc/1/maps")
        assert "m_javascript.so" not in maps, "JavaScript must be unloaded by default"
        assert "m_precommand.so" not in maps, "Unredacted observer must be unloaded by default"
        docker(
            "exec",
            container,
            "sh",
            "-ec",
            'printf \'\\n# persisted packaging fixture\\n<module name="javascript">\\n'
            '<javascript file="packaging.js">\\n\' >> /inspircd/conf/inspircd.conf; '
            "printf '// Empty deployment-owned script.\\n' > /inspircd/conf/packaging.js",
        )
        config = docker("exec", container, "cat", "/inspircd/conf/inspircd.conf")
        docker("restart", container)
        check_irc(container)
        assert docker("exec", container, "cat", "/inspircd/conf/inspircd.conf") == config
        assert docker("exec", container, "cat", "/inspircd/conf/packaging.js") == (
            "// Empty deployment-owned script.\n"
        )
        maps = docker("exec", container, "cat", "/proc/1/maps")
        assert "m_javascript.so" in maps, "Explicit configuration must load the packaged module"
        assert "libnode.so.127" in maps, "Loaded module must use the packaged V8 library"
        assert "m_precommand.so" not in maps
        check_nocodb_activation(image)
        check_persistent_volume(image)
        print("PASS: primary image, opt-in JavaScript, persistent config, and ordinary IRC")
    except Exception:
        if container is not None:
            print(docker("logs", container))
        raise
    finally:
        if container is not None:
            docker("rm", "-f", container)
        docker("volume", "rm", volume)


if __name__ == "__main__":
    main()
