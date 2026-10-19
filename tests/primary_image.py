#!/usr/bin/env python3
"""Smoke-test primary-image packaging with disposable configuration and no policy hooks."""

import subprocess
import uuid

from precommand_client import Client


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
