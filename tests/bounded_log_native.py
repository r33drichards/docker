#!/usr/bin/env python3
"""Actual-daemon logging regression with the production module, identity and paths.

Prerequisites: Linux unprivileged user namespaces, util-linux unshare, Python 3,
ldd, a runnable InspIRCd install, and both modules built from the same sources.
No Docker, root account, network beyond loopback, or production test switches.
The chroot is created inside a temporary fixture and removed after the test.

Prepare the test-only probe (never copy it into a normal production image):
  cp tests/fixtures/m_bounded_log_probe.cpp "$INSPIRCD_SOURCE/src/modules/"
  cp modules/{m_log_bounded.cpp,bounded_log.h} "$INSPIRCD_SOURCE/src/modules/"
  INSPIRCD_TARGET="m_log_bounded m_bounded_log_probe" make -C "$INSPIRCD_SOURCE"

Run with the actual install and matching build output directories:
  INSPIRCD_NATIVE=/path/to/install INSPIRCD_TEST_MODULES=/path/to/build/modules \
    unshare --user --map-user=10000 --map-group=10000 --keep-caps \
    python3 tests/bounded_log_native.py

User namespaces are required, not silently skipped if unavailable. LeakSanitizer
is unrelated to this smoke; use tests/bounded_log.sh for sanitizer checks.
"""

import os
import pathlib
import re
import shutil
import signal
import socket
import subprocess
import tempfile
import time

assert os.getuid() == 10000 and os.getgid() == 10000, (
    "Run through the documented user namespace command"
)
run = pathlib.Path(os.environ["INSPIRCD_NATIVE"]).resolve()
modules = pathlib.Path(os.environ["INSPIRCD_TEST_MODULES"]).resolve()
with tempfile.TemporaryDirectory(prefix="bounded-runtime-") as temporary:
    root = pathlib.Path(temporary)
    for directory, mode in [
        ("inspircd", 0o755),
        ("inspircd/modules", 0o755),
        ("inspircd/bin", 0o755),
        ("inspircd/conf", 0o700),
        ("inspircd/data", 0o700),
        ("inspircd/persistent", 0o755),
        ("inspircd/persistent/logs", 0o700),
        ("etc", 0o755),
    ]:
        (root / directory).mkdir(mode=mode, parents=True, exist_ok=True)
    shutil.copy2(run / "bin/inspircd", root / "inspircd/bin/inspircd")
    for module in (run / "modules").glob("core*.so"):
        shutil.copy2(module, root / "inspircd/modules" / module.name)
    for module in ["m_log_bounded.so", "m_bounded_log_probe.so"]:
        shutil.copy2(modules / module, root / "inspircd/modules" / module)
    dependencies = subprocess.check_output(["ldd", str(run / "bin/inspircd")], text=True)
    for dependency in re.findall(r"(?:=>\s+)?(/[^\s]+)", dependencies):
        path = root / dependency.lstrip("/")
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(dependency, path)
    (root / "etc/passwd").write_text("inspircd:x:10000:10000::/inspircd:/bin/false\n")
    (root / "etc/group").write_text("inspircd:x:10000:\n")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    config = root / "inspircd/conf/inspircd.conf"
    config.write_text(f'''<server name="bounded.test" description="Synthetic bounded log smoke" id="001" network="BoundedTest">
<admin name="Test" email="test@example.invalid">
<bind address="127.0.0.1" port="{port}" type="clients">
<connect name="test" allow="127.0.0.1" maxlocal="20" maxglobal="20">
<path configdir="/inspircd/conf" moduledir="/inspircd/modules" datadir="/inspircd/data" logdir="/inspircd/persistent/logs" runtimedir="/inspircd/data">
<pid file="/inspircd/data/inspircd.pid">
<module name="log_bounded">
<module name="bounded_log_probe">
<log method="bounded" level="normal" type="* -USERINPUT -USEROUTPUT" maxbytes="1024" files="5">
''')
    output_path = root / "stdout.log"
    logdir = root / "inspircd/persistent/logs"

    def child():
        os.chroot(root)
        os.chdir("/inspircd")

    def records():
        result = ""
        total = 0
        files = list(logdir.glob("inspircd.log*"))
        assert len(files) <= 6, [file.name for file in files]
        for path in files:
            size = path.stat().st_size
            assert size <= 1024, (path.name, size)
            total += size
            result += path.read_text()
        assert total <= 5 * 1024, total
        assert "TOP-SECRET" not in result, result
        return result, total

    def lasttick():
        values = re.findall(r"probe (\d+) ", records()[0])
        return max(map(int, values)) if values else 0

    def wait_for(predicate, description, seconds=10):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            assert process.poll() is None, output_path.read_text()
            if predicate():
                return
            time.sleep(0.05)
        raise AssertionError(description + "\n" + output_path.read_text())

    with output_path.open("w") as output:
        process = subprocess.Popen(
            ["/inspircd/bin/inspircd", "--nofork", "--config", "/inspircd/conf/inspircd.conf"],
            preexec_fn=child,
            stdout=output,
            stderr=subprocess.STDOUT,
        )
        try:
            wait_for(lambda: lasttick() >= 2, "probe did not start")
            before = lasttick()
            process.send_signal(signal.SIGHUP)
            wait_for(lambda: lasttick() >= before + 2, "logging did not continue after rehash")
            assert "active writer" not in output_path.read_text(), output_path.read_text()
            assert "Cannot open log files" not in output_path.read_text(), output_path.read_text()
            print(
                "PASS: exact native module startup, bounded burst and rehash", records()[1], "bytes"
            )
            (logdir / "inspircd.log").chmod(0o644)
            wait_for(
                lambda: "Bounded logging disabled:" in output_path.read_text(),
                "runtime failure not visible",
            )
            snapshot = records()[0]
            time.sleep(2)
            assert snapshot == records()[0], "disabled logger kept writing"
            assert output_path.read_text().count("Bounded logging disabled:") == 1, (
                output_path.read_text()
            )
            assert process.poll() is None, "logger failure killed daemon"
            (logdir / "inspircd.log").chmod(0o600)
            before = lasttick()
            process.send_signal(signal.SIGHUP)
            wait_for(lambda: lasttick() > before, "repair plus rehash did not restore logging")
            print(
                "PASS: nonfatal runtime failure disables once; repair plus rehash restores logging",
                records()[1],
                "bytes",
            )
            original = config.read_text()
            diagnostic = "Bounded logging initialization failed;"
            config.write_text(original.replace('maxbytes="1024"', 'maxbytes="1023"'))
            process.send_signal(signal.SIGHUP)
            wait_for(
                lambda: output_path.read_text().count(diagnostic) == 1,
                "invalid limit during rehash was not visible",
            )
            snapshot = records()[0]
            time.sleep(2)
            assert snapshot == records()[0], "invalid replacement kept writing"
            config.write_text(original)
            (logdir / "inspircd.log").chmod(0o644)
            process.send_signal(signal.SIGHUP)
            wait_for(
                lambda: output_path.read_text().count(diagnostic) == 2,
                "unsafe existing file during rehash was not visible",
            )
            assert process.poll() is None, "invalid logger replacement killed daemon"
            (logdir / "inspircd.log").chmod(0o600)
            before = lasttick()
            process.send_signal(signal.SIGHUP)
            wait_for(lambda: lasttick() > before, "failed replacement could not recover")
            print(
                "PASS: invalid-limit and unsafe-file rehash failures remain visible and recoverable"
            )
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
