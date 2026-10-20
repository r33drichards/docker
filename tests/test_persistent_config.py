"""Exercise the real fixed-path C helper in an isolated compiled filesystem root."""

import hashlib
import os
import shlex
import shutil
import signal
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tools/seed-persistent-config.c"
MANIFEST = ".seed-persistent-config.manifest"
JOURNAL = ".seed-persistent-config.journal"
LOCK = ".seed-persistent-config.lock"
CONFIG_STAGE = ".seed-persistent-config.stage"
METADATA_STAGE = ".seed-persistent-config.metadata-stage"
READY = ".migration-ready"
HEADER = "seed-persistent-config-v1\n"
SECRET = "private-test-certificate-and-token-marker"


def digest(value):
    return hashlib.sha256(value.encode()).hexdigest()


class PersistentConfigTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.fixture = Path(cls.tmp.name)
        cls.fixture.chmod(0o755)
        cls.uid = 10000 if os.geteuid() == 0 else os.getuid()
        cls.gid = 10000 if os.geteuid() == 0 else os.getgid()
        cls.binaries = {}
        source = (
            SOURCE.read_text()
            .replace('open("/",', f'open("{cls.fixture}/",')
            .replace("daemon_uid = 10000", f"daemon_uid = {cls.uid}")
            .replace("daemon_gid = 10000", f"daemon_gid = {cls.gid}")
        )
        flags = shlex.split(
            subprocess.check_output(
                ["pkg-config", "--static", "--libs", "libcrypto"], text=True
            ).strip()
        )
        for point in (None, "JOURNAL", "PUBLISH", "MANIFEST", "TIMEOUT"):
            code = source
            if point:
                # Failure injection exists only in this private test copy. No
                # production CLI, environment override, or failpoint is added.
                code = code.replace(f"/* TEST-FAILPOINT-AFTER-{point} */", "_exit(91);")
            if point == "TIMEOUT":
                code = code.replace("import_timeout_seconds = 1800", "import_timeout_seconds = 1")
            test_source = cls.fixture / f"helper-{point}.c"
            test_source.write_text(code)
            binary = cls.fixture / f"helper-{point}"
            subprocess.run(
                [
                    os.environ.get("CC", "cc"),
                    "-std=c11",
                    "-Os",
                    "-static",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(test_source),
                    *flags,
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                check=True,
            )
            cls.binaries[point] = binary

    def setUp(self):
        self.source = self.fixture / "conf"
        self.source.mkdir(mode=0o755)
        self.inspircd = self.fixture / "inspircd"
        self.inspircd.mkdir(mode=0o755)
        self.mount = self.inspircd / "persistent"
        self.mount.mkdir(mode=0o755)
        self.destination = self.mount / "conf"
        self.destination.mkdir(mode=0o700)
        self.own(self.destination)
        self.baseline = self.fixture / "seed-persistent-config.baseline"
        self.outside = self.fixture / "outside"
        self.outside.mkdir(mode=0o755)
        self.put_source("inspircd.conf", "initial config\n")
        self.put_source("config.sh", "#!/bin/sh\necho initial\n", 0o755)
        self.addCleanup(self.cleanup)

    def cleanup(self):
        for path in (self.source, self.inspircd, self.outside):
            if path.is_symlink():
                path.unlink()
            elif path.exists():
                for base, dirs, _ in os.walk(path):
                    Path(base).chmod(0o755)
                    for name in dirs:
                        child = Path(base) / name
                        if not child.is_symlink():
                            child.chmod(0o755)
                shutil.rmtree(path)
        if self.baseline.is_symlink() or self.baseline.exists():
            self.baseline.unlink()
        self.fixture.chmod(0o755)

    def own(self, path):
        if os.geteuid() == 0:
            os.chown(path, self.uid, self.gid)

    def put_source(self, name, content, mode=0o644):
        path = self.source / name
        path.write_text(content)
        path.chmod(mode)
        return path

    def put_destination(self, name, content, mode=0o644):
        path = self.destination / name
        path.write_text(content)
        self.own(path)
        path.chmod(mode)
        return path

    def make_baseline(self, records):
        self.baseline.write_text(
            HEADER + "".join(f"{digest(value)} {name}\n" for name, value in records.items())
        )
        self.baseline.chmod(0o644)

    def start_helper(self, point=None, *, root=False, args=(), env=None):
        def drop_privileges():
            os.setgroups([])
            os.setgid(self.gid)
            os.setuid(self.uid)

        environment = os.environ.copy()
        environment.pop("INSPIRCD_PERSISTENT_IMPORT", None)
        environment.update(env or {})
        return subprocess.Popen(
            [str(self.binaries[point]), *args],
            env=environment,
            preexec_fn=drop_privileges if os.geteuid() == 0 and not root else None,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

    def run_helper(self, point=None, *, root=False, args=(), env=None):
        process = self.start_helper(point, root=root, args=args, env=env)
        try:
            stdout, stderr = process.communicate(timeout=10)
        except BaseException:
            process.kill()
            process.communicate()
            raise
        result = subprocess.CompletedProcess(process.args, process.returncode, stdout, stderr)
        self.assertNotIn(SECRET, result.stdout + result.stderr)
        self.assertEqual(result.stdout, "")
        return result

    def prepare_import(self, key=SECRET + "-key", cert=SECRET + "-cert"):
        self.put_destination("inspircd.conf", "imported main configuration\n", 0o600)
        self.put_destination("key.pem", key, 0o600)
        self.put_destination("cert.pem", cert, 0o644)
        upload = self.put_destination(
            ".readiness-upload",
            f"persistent-import-v1\n{digest(key)} key.pem\n{digest(cert)} cert.pem\n",
            0o600,
        )
        upload.replace(self.destination / READY)

    def import_environment(self):
        return {"INSPIRCD_PERSISTENT": "yes", "INSPIRCD_PERSISTENT_IMPORT": "yes"}

    def assert_ok(self, result):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")

    def assert_refused(self, result):
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Persistent configuration:", result.stderr)

    def read_manifest(self):
        lines = (self.destination / MANIFEST).read_text().splitlines()
        self.assertEqual(lines[0], HEADER.strip())
        return {name: hash_ for hash_, name in (line.split(" ") for line in lines[1:])}

    def test_first_seed_preserves_modes_and_private_extras(self):
        key = self.put_destination("key.pem", SECRET, 0o600)
        custom = self.put_destination("custom.conf", SECRET, 0o600)
        private_dir = self.destination / "private"
        private_dir.mkdir(mode=0o700)
        self.own(private_dir)
        external = self.outside / "secret"
        external.write_text(SECRET)
        (self.destination / "external-secret").symlink_to(external)
        before = {path: path.stat() for path in (key, custom, external)}
        self.assert_ok(self.run_helper())
        for name in ("inspircd.conf", "config.sh"):
            self.assertEqual(
                (self.destination / name).read_bytes(), (self.source / name).read_bytes()
            )
        self.assertEqual((self.destination / "inspircd.conf").stat().st_mode & 0o777, 0o644)
        self.assertEqual((self.destination / "config.sh").stat().st_mode & 0o777, 0o755)
        self.assertEqual(self.destination.stat().st_mode & 0o777, 0o700)
        for path, prior in before.items():
            self.assertEqual(path.read_text(), SECRET)
            self.assertEqual(path.stat().st_ctime_ns, prior.st_ctime_ns)
        self.assertEqual(set(self.read_manifest()), {"config.sh", "inspircd.conf"})
        metadata = (self.destination / MANIFEST).read_text()
        self.assertNotIn(SECRET, metadata)
        self.assertNotIn(digest(SECRET), metadata)
        self.assertFalse((self.destination / JOURNAL).exists())
        for name in (LOCK, MANIFEST):
            self.assertEqual((self.destination / name).stat().st_mode & 0o777, 0o600)

    def test_update_only_unedited_managed_files_and_preserves_private_mode(self):
        self.assert_ok(self.run_helper())
        self.put_destination("inspircd.conf", "my edited config\n", 0o600)
        script = self.destination / "config.sh"
        script.chmod(0o700)
        self.put_source("inspircd.conf", "new image config\n")
        self.put_source("config.sh", "#!/bin/sh\necho upgraded\n", 0o755)
        self.assert_ok(self.run_helper())
        self.assertEqual((self.destination / "inspircd.conf").read_text(), "my edited config\n")
        self.assertEqual(script.read_text(), "#!/bin/sh\necho upgraded\n")
        self.assertEqual(script.stat().st_mode & 0o777, 0o700)
        self.assertEqual(self.read_manifest()["inspircd.conf"], digest("initial config\n"))
        self.assert_ok(self.run_helper())
        self.assertEqual((self.destination / "inspircd.conf").read_text(), "my edited config\n")

    def test_existing_unmanaged_files_are_not_adopted_even_if_matching(self):
        self.put_destination("inspircd.conf", "initial config\n", 0o600)
        self.assert_ok(self.run_helper())
        self.assertNotIn("inspircd.conf", self.read_manifest())
        self.put_source("inspircd.conf", "new image\n")
        self.assert_ok(self.run_helper())
        self.assertEqual((self.destination / "inspircd.conf").read_text(), "initial config\n")
        self.assertEqual((self.destination / "inspircd.conf").stat().st_mode & 0o777, 0o600)

    def test_baseline_adopts_only_exact_existing_known_defaults(self):
        self.put_destination("inspircd.conf", "old deployed image\n", 0o600)
        self.put_destination("config.sh", "custom script\n", 0o700)
        self.put_destination("unbundled.conf", SECRET, 0o600)
        self.make_baseline(
            {
                "inspircd.conf": "old deployed image\n",
                "config.sh": "old script\n",
                "unbundled.conf": SECRET,
            }
        )
        self.assert_ok(self.run_helper())
        self.assertEqual((self.destination / "inspircd.conf").read_text(), "initial config\n")
        self.assertEqual((self.destination / "inspircd.conf").stat().st_mode & 0o777, 0o600)
        self.assertEqual((self.destination / "config.sh").read_text(), "custom script\n")
        self.assertEqual((self.destination / "unbundled.conf").read_text(), SECRET)
        self.assertEqual(self.read_manifest(), {"inspircd.conf": digest("initial config\n")})

    def test_baseline_does_not_prevent_seeding_missing_files(self):
        self.make_baseline({"inspircd.conf": "old image", "config.sh": "old script"})
        self.assert_ok(self.run_helper())
        self.assertEqual(set(self.read_manifest()), {"inspircd.conf", "config.sh"})

    def test_baseline_is_not_reapplied_after_initialization(self):
        self.assert_ok(self.run_helper())
        self.put_destination("inspircd.conf", "my change")
        self.make_baseline({"inspircd.conf": "my change"})
        self.put_source("inspircd.conf", "new image")
        self.assert_ok(self.run_helper())
        self.assertEqual((self.destination / "inspircd.conf").read_text(), "my change")

    def test_invalid_baseline_path_or_tls_names_fail_without_touching_files(self):
        for name in ("../outside/secret", "/outside/secret", "key.pem", "cert.pem", "dhparams.pem"):
            with self.subTest(name=name):
                self.make_baseline({name: SECRET})
                self.assert_refused(self.run_helper())
                self.assertFalse((self.destination / "inspircd.conf").exists())

    def test_baseline_symlink_hardlink_and_writable_modes_refused(self):
        external = self.outside / "baseline"
        external.write_text(HEADER)
        self.baseline.symlink_to(external)
        self.assert_refused(self.run_helper())
        self.baseline.unlink()
        self.baseline.hardlink_to(external)
        self.assert_refused(self.run_helper())
        self.baseline.unlink()
        self.make_baseline({})
        self.baseline.chmod(0o666)
        self.assert_refused(self.run_helper())

    def test_new_image_files_seed_only_when_absent(self):
        self.assert_ok(self.run_helper())
        self.put_source("added.conf", "new default")
        self.put_source("custom.conf", "colliding default")
        self.put_destination("custom.conf", "user custom", 0o600)
        self.assert_ok(self.run_helper())
        self.assertEqual((self.destination / "added.conf").read_text(), "new default")
        self.assertEqual((self.destination / "custom.conf").read_text(), "user custom")
        self.assertNotIn("custom.conf", self.read_manifest())

    def test_removed_image_files_become_unmanaged_without_deletion(self):
        self.assert_ok(self.run_helper())
        (self.source / "config.sh").unlink()
        self.assert_ok(self.run_helper())
        self.assertTrue((self.destination / "config.sh").is_file())
        self.assertNotIn("config.sh", self.read_manifest())
        self.put_source("config.sh", "reintroduced image")
        self.assert_ok(self.run_helper())
        self.assertEqual((self.destination / "config.sh").read_text(), "#!/bin/sh\necho initial\n")

    def test_deleted_managed_file_is_not_recreated(self):
        self.assert_ok(self.run_helper())
        (self.destination / "config.sh").unlink()
        self.put_source("config.sh", "new image")
        self.assert_ok(self.run_helper())
        self.assertFalse((self.destination / "config.sh").exists())

    def test_source_symlink_hardlink_directory_and_fifo_refused(self):
        target = self.source / "attack.conf"
        external = self.outside / "secret"
        external.write_text(SECRET)
        target.symlink_to(external)
        self.assert_refused(self.run_helper())
        target.unlink()
        target.hardlink_to(external)
        self.assert_refused(self.run_helper())
        target.unlink()
        target.mkdir()
        self.assert_refused(self.run_helper())
        target.rmdir()
        os.mkfifo(target)
        self.assert_refused(self.run_helper())
        self.assertFalse((self.destination / "inspircd.conf").exists())
        self.assertEqual(external.read_text(), SECRET)

    def test_source_filename_controls_refused(self):
        self.put_source("bad\nname", SECRET)
        self.assert_refused(self.run_helper())
        self.assertFalse((self.destination / "inspircd.conf").exists())

    def test_destination_symlink_hardlink_directory_and_fifo_refused(self):
        target = self.destination / "inspircd.conf"
        external = self.outside / "secret"
        external.write_text(SECRET)
        target.symlink_to(external)
        self.assert_refused(self.run_helper())
        target.unlink()
        target.hardlink_to(external)
        self.assert_refused(self.run_helper())
        target.unlink()
        target.mkdir()
        self.assert_refused(self.run_helper())
        target.rmdir()
        os.mkfifo(target)
        self.assert_refused(self.run_helper())
        self.assertFalse((self.destination / "config.sh").exists())
        self.assertEqual(external.read_text(), SECRET)

    def test_each_fixed_directory_rejects_symlinks(self):
        for target in (self.source, self.destination, self.mount, self.inspircd):
            with self.subTest(target=target.name):
                moved = self.outside / "moved"
                target.rename(moved)
                target.symlink_to(moved, target_is_directory=True)
                self.assert_refused(self.run_helper())
                target.unlink()
                moved.rename(target)

    def test_private_directory_mode_is_not_silently_changed(self):
        for mode in (0o755, 0o770, 0o777, 0o1700):
            with self.subTest(mode=oct(mode)):
                self.destination.chmod(mode)
                self.assert_refused(self.run_helper())
                self.assertEqual(self.destination.stat().st_mode & 0o7777, mode)
        self.destination.chmod(0o700)

    def test_metadata_symlinks_and_hardlinks_refused(self):
        external = self.outside / "secret"
        external.write_text(SECRET)
        for name in (MANIFEST, JOURNAL, LOCK):
            with self.subTest(name=name):
                target = self.destination / name
                if target.exists():
                    target.unlink()
                target.symlink_to(external)
                self.assert_refused(self.run_helper())
                target.unlink()
                target.hardlink_to(external)
                self.assert_refused(self.run_helper())
                target.unlink()
        self.assertEqual(external.read_text(), SECRET)

    def test_corrupt_manifest_is_not_rebuilt_or_used(self):
        self.put_destination(MANIFEST, HEADER + "bad hash ../outside/secret\n", 0o600)
        self.assert_refused(self.run_helper())
        self.assertFalse((self.destination / "inspircd.conf").exists())

    def test_failure_after_journal_publish_or_manifest_is_recoverable(self):
        self.assert_ok(self.run_helper())
        for point in ("JOURNAL", "PUBLISH", "MANIFEST"):
            with self.subTest(point=point):
                content = f"updated {point}\n"
                self.put_source("config.sh", content)
                result = self.run_helper(point)
                self.assertEqual(result.returncode, 91, result.stderr)
                self.assertTrue((self.destination / JOURNAL).exists())
                # The current on-disk manifest is always complete and parseable.
                self.read_manifest()
                self.assert_ok(self.run_helper())
                self.assertEqual((self.destination / "config.sh").read_text(), content)
                self.assertEqual(self.read_manifest()["config.sh"], digest(content))
                self.assertFalse((self.destination / JOURNAL).exists())

    def test_user_edit_after_failed_upgrade_is_preserved_on_retry(self):
        self.assert_ok(self.run_helper())
        for point in ("JOURNAL", "PUBLISH", "MANIFEST"):
            with self.subTest(point=point):
                # Restore the currently tracked exact version for the next case.
                prior = (self.source / "config.sh").read_text()
                self.put_destination("config.sh", prior)
                self.put_source("config.sh", f"upgrade {point}")
                self.assertEqual(self.run_helper(point).returncode, 91)
                self.put_destination("config.sh", "user intervened " + SECRET, 0o600)
                self.assert_ok(self.run_helper())
                self.assertEqual(
                    (self.destination / "config.sh").read_text(), "user intervened " + SECRET
                )
                # Prepare the next case without altering this case's conclusion.
                self.put_destination("config.sh", (self.source / "config.sh").read_text())
                manifest = self.read_manifest()
                manifest["config.sh"] = digest((self.source / "config.sh").read_text())
                self.put_destination(
                    MANIFEST,
                    HEADER + "".join(f"{hash_} {name}\n" for name, hash_ in manifest.items()),
                    0o600,
                )

    def test_new_file_interrupted_before_publication_never_overwrites_racing_file(self):
        self.assert_ok(self.run_helper())
        self.put_source("added.conf", "new bundled")
        self.assertEqual(self.run_helper("JOURNAL").returncode, 91)
        self.put_destination("added.conf", "arrived during failure", 0o600)
        self.assert_ok(self.run_helper())
        self.assertEqual((self.destination / "added.conf").read_text(), "arrived during failure")
        self.assertNotIn("added.conf", self.read_manifest())

    def test_new_file_interrupted_after_publication_retains_managed_tracking(self):
        self.assert_ok(self.run_helper())
        self.put_source("added.conf", "new bundled")
        self.assertEqual(self.run_helper("PUBLISH").returncode, 91)
        self.assert_ok(self.run_helper())
        self.assertEqual(self.read_manifest()["added.conf"], digest("new bundled"))
        self.put_source("added.conf", "next bundled")
        self.assert_ok(self.run_helper())
        self.assertEqual((self.destination / "added.conf").read_text(), "next bundled")

    def test_crashed_update_and_newer_image_recover_in_sequence(self):
        self.assert_ok(self.run_helper())
        self.put_source("config.sh", "intermediate")
        self.assertEqual(self.run_helper("PUBLISH").returncode, 91)
        self.put_source("config.sh", "newest image")
        self.assert_ok(self.run_helper())
        self.assertEqual((self.destination / "config.sh").read_text(), "newest image")

    def test_removed_pending_file_is_never_read_or_adopted_on_recovery(self):
        self.assert_ok(self.run_helper())
        self.put_source("config.sh", "intermediate")
        self.assertEqual(self.run_helper("PUBLISH").returncode, 91)
        (self.source / "config.sh").unlink()
        (self.destination / "config.sh").unlink()
        external = self.outside / "secret"
        external.write_text(SECRET)
        (self.destination / "config.sh").symlink_to(external)
        self.assert_ok(self.run_helper())
        self.assertEqual(external.read_text(), SECRET)
        self.assertNotIn("config.sh", self.read_manifest())

    def test_arguments_rejected_and_environment_root_overrides_ignored(self):
        self.assert_refused(self.run_helper(args=(str(self.outside),)))
        self.assert_ok(
            self.run_helper(
                env={
                    "INSPIRCD_ROOT": str(self.outside),
                    "INSPIRCD_PERSISTENT_ROOT": str(self.outside),
                    "SEED_CONFIG_ROOT": str(self.outside),
                    "INSPIRCD_NOCODB_TOKEN": SECRET,
                }
            )
        )
        self.assertEqual(list(self.outside.iterdir()), [])

    @unittest.skipUnless(os.geteuid() == 0, "root refusal requires real root")
    def test_root_is_refused_before_any_write(self):
        self.assert_refused(self.run_helper(root=True))
        self.assertEqual(list(self.destination.iterdir()), [])

    def test_import_wait_does_not_write_until_atomic_ready_marker(self):
        process = self.start_helper(env=self.import_environment())
        try:
            notice = process.stderr.readline()
            self.assertEqual(notice, "Persistent configuration: waiting for approved TLS import.\n")
            self.assertIsNone(process.poll())
            self.assertEqual(list(self.destination.iterdir()), [])
            self.prepare_import()
            stdout, stderr = process.communicate(timeout=4)
            self.assertEqual(process.returncode, 0, stderr)
            self.assertEqual(stdout + stderr, "")
            self.assertEqual((self.destination / "key.pem").read_text(), SECRET + "-key")
            self.assertTrue((self.destination / MANIFEST).exists())
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate()

    def test_import_marker_pins_tls_on_every_start(self):
        self.prepare_import()
        self.assert_ok(self.run_helper(env=self.import_environment()))
        before = (self.destination / MANIFEST).read_bytes()
        self.put_destination("cert.pem", SECRET + "-changed-cert", 0o644)
        self.assert_refused(self.run_helper(env=self.import_environment()))
        self.assertEqual((self.destination / MANIFEST).read_bytes(), before)
        # Explicit stopped maintenance: replace files, then publish new marker.
        self.prepare_import(SECRET + "-rotated-key", SECRET + "-rotated-cert")
        self.assert_ok(self.run_helper(env=self.import_environment()))

    def test_import_timeout_and_sigterm_leave_configuration_unseeded(self):
        result = self.run_helper("TIMEOUT", env=self.import_environment())
        self.assert_refused(result)
        self.assertIn("deadline expired", result.stderr)
        self.assertEqual(result.stderr.count("waiting for approved TLS import"), 1)
        self.assertEqual(list(self.destination.iterdir()), [])
        process = self.start_helper(env=self.import_environment())
        try:
            self.assertIn("waiting for approved TLS import", process.stderr.readline())
            process.send_signal(signal.SIGTERM)
            stdout, stderr = process.communicate(timeout=3)
            self.assertNotEqual(process.returncode, 0)
            self.assertEqual(stdout, "")
            self.assertIn("interrupted", stderr)
            self.assertEqual(list(self.destination.iterdir()), [])
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate()

    def test_import_flag_validation_fails_before_any_write(self):
        for value in ("YES", "true", "1", "yes\n", SECRET):
            with self.subTest(value=value):
                self.assert_refused(self.run_helper(env={"INSPIRCD_PERSISTENT_IMPORT": value}))
                self.assertEqual(list(self.destination.iterdir()), [])
        for persistent in ("", "no", "YES"):
            with self.subTest(persistent=persistent):
                self.assert_refused(
                    self.run_helper(
                        env={"INSPIRCD_PERSISTENT_IMPORT": "yes", "INSPIRCD_PERSISTENT": persistent}
                    )
                )
                self.assertEqual(list(self.destination.iterdir()), [])
        for value in ("", "no"):
            self.assert_ok(self.run_helper(env={"INSPIRCD_PERSISTENT_IMPORT": value}))

    def test_import_missing_required_files_and_empty_files_fail(self):
        for name in ("key.pem", "cert.pem", "inspircd.conf"):
            with self.subTest(name=name):
                self.prepare_import()
                (self.destination / name).unlink()
                self.assert_refused(self.run_helper(env=self.import_environment()))
                self.put_destination(name, "", 0o600)
                self.assert_refused(self.run_helper(env=self.import_environment()))
                self.assertFalse((self.destination / LOCK).exists())

    def test_import_unsafe_marker_and_file_links_fail_without_touching_targets(self):
        external = self.outside / "secret"
        external.write_text(SECRET)
        for name in (READY, "key.pem", "cert.pem", "inspircd.conf"):
            with self.subTest(name=name):
                self.prepare_import()
                target = self.destination / name
                target.unlink()
                target.symlink_to(external)
                self.assert_refused(self.run_helper(env=self.import_environment()))
                target.unlink()
                target.hardlink_to(external)
                self.assert_refused(self.run_helper(env=self.import_environment()))
                target.unlink()
                self.assertEqual(external.read_text(), SECRET)
                self.assertFalse((self.destination / LOCK).exists())

    def test_import_bad_modes_or_malformed_marker_fail_closed(self):
        for name, mode in (
            (READY, 0o644),
            ("key.pem", 0o644),
            ("cert.pem", 0o666),
            ("cert.pem", 0o4644),
            ("inspircd.conf", 0o666),
        ):
            with self.subTest(name=name, mode=mode):
                self.prepare_import()
                (self.destination / name).chmod(mode)
                self.assert_refused(self.run_helper(env=self.import_environment()))
                self.assertFalse((self.destination / LOCK).exists())
        self.prepare_import()
        valid = (self.destination / READY).read_text()
        for content in (
            valid.rstrip(),
            valid + "extra\n",
            valid.replace("key.pem", "other.pem"),
            valid.replace(digest(SECRET + "-key"), "0" * 64),
            SECRET,
        ):
            with self.subTest(content_length=len(content)):
                self.put_destination(READY, content, 0o600)
                self.assert_refused(self.run_helper(env=self.import_environment()))
                self.assertFalse((self.destination / LOCK).exists())

    @unittest.skipUnless(os.geteuid() == 0, "ownership checks require root")
    def test_import_wrong_uid_or_gid_is_refused(self):
        for name in (READY, "key.pem", "cert.pem", "inspircd.conf"):
            for uid, gid in ((0, self.gid), (self.uid, 0)):
                with self.subTest(name=name, uid=uid, gid=gid):
                    self.prepare_import()
                    os.chown(self.destination / name, uid, gid)
                    self.assert_refused(self.run_helper(env=self.import_environment()))
                    self.assertFalse((self.destination / LOCK).exists())

    def test_repeated_interrupted_updates_have_only_bounded_fixed_staging(self):
        self.assert_ok(self.run_helper())
        self.put_source("config.sh", "x" * (1024 * 1024))
        for _ in range(5):
            self.assertEqual(self.run_helper("JOURNAL").returncode, 91)
            stages = [
                p for p in self.destination.iterdir() if p.name in (CONFIG_STAGE, METADATA_STAGE)
            ]
            self.assertEqual([p.name for p in stages], [CONFIG_STAGE])
            self.assertLessEqual(sum(p.stat().st_size for p in stages), 1024 * 1024)
            self.assertEqual(list(self.destination.glob(".seed-persistent-config.tmp.*")), [])
        self.assert_ok(self.run_helper())
        self.assertFalse((self.destination / CONFIG_STAGE).exists())
        self.assertFalse((self.destination / METADATA_STAGE).exists())

    def test_safe_abandoned_stages_cleaned_but_similarly_named_user_files_preserved(self):
        self.put_destination(CONFIG_STAGE, "unfinished configuration", 0o700)
        self.put_destination(METADATA_STAGE, "incomplete metadata", 0o600)
        names = [
            ".seed-persistent-config.tmp.user-data",
            CONFIG_STAGE + ".custom",
            METADATA_STAGE + ".backup",
        ]
        for name in names:
            self.put_destination(name, SECRET, 0o600)
        self.assert_ok(self.run_helper())
        self.assertFalse((self.destination / CONFIG_STAGE).exists())
        self.assertFalse((self.destination / METADATA_STAGE).exists())
        for name in names:
            self.assertEqual((self.destination / name).read_text(), SECRET)

    def test_destination_fixture_applies_special_modes_after_ownership(self):
        # chown clears set-ID bits, even when changing to the same owner. The
        # root CI fixture must exercise the intended unsafe mode, not a safe file.
        self.own = lambda path: os.chown(path, os.getuid(), os.getgid())
        path = self.put_destination(CONFIG_STAGE, SECRET, 0o4600)
        self.assertEqual(path.stat().st_mode & 0o7777, 0o4600)

    def test_unsafe_fixed_stages_are_refused_not_deleted(self):
        external = self.outside / "secret"
        external.write_text(SECRET)
        for name in (CONFIG_STAGE, METADATA_STAGE):
            with self.subTest(name=name):
                target = self.destination / name
                target.symlink_to(external)
                self.assert_refused(self.run_helper())
                self.assertTrue(target.is_symlink())
                target.unlink()
                target.hardlink_to(external)
                self.assert_refused(self.run_helper())
                self.assertEqual(target.stat().st_nlink, 2)
                target.unlink()
                self.put_destination(name, SECRET, 0o4600)
                self.assert_refused(self.run_helper())
                self.assertTrue(target.exists())
                target.unlink()
                self.assertEqual(external.read_text(), SECRET)
        oversized = self.put_destination(CONFIG_STAGE, "", 0o600)
        with oversized.open("wb") as handle:
            handle.truncate(16 * 1024 * 1024 + 1)
        self.assert_refused(self.run_helper())
        self.assertTrue(oversized.exists())

    def test_entrypoint_forwards_term_to_waiter_and_never_runs_next_step(self):
        script = (ROOT / "entrypoint.sh").read_text()
        script = script.split("# Link certificates from secrets", 1)[0]
        script = script.replace(". /materialize-nocodb-token.sh || exit 1", ":")
        script = script.replace("/seed-persistent-config &", f'"{self.binaries[None]}" &')
        script += "\nprintf 'UNSAFE-DAEMON-START\\n'\n"
        entrypoint = self.fixture / "gated-entrypoint.sh"
        entrypoint.write_text(script)
        entrypoint.chmod(0o755)
        environment = {**os.environ, **self.import_environment()}

        def drop_privileges():
            os.setgroups([])
            os.setgid(self.gid)
            os.setuid(self.uid)

        process = subprocess.Popen(
            ["sh", str(entrypoint)],
            env=environment,
            preexec_fn=drop_privileges if os.geteuid() == 0 else None,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            self.assertIn("waiting for approved TLS import", process.stderr.readline())
            process.terminate()
            stdout, stderr = process.communicate(timeout=4)
            self.assertNotEqual(process.returncode, 0)
            self.assertNotIn("UNSAFE-DAEMON-START", stdout + stderr)
            self.assertEqual(list(self.destination.iterdir()), [])
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate()

    def test_entrypoint_rejects_import_bypass_before_running_any_helper(self):
        script = (ROOT / "entrypoint.sh").read_text()
        script = script.split("# Link certificates from secrets", 1)[0]
        script = script.replace(". /materialize-nocodb-token.sh || exit 1", ":")
        script = script.replace("/seed-persistent-config &", "printf 'UNSAFE-HELPER-START' &")
        script += "\nprintf 'UNSAFE-DAEMON-START\\n'\n"
        for persistent, importing in (("no", "yes"), ("", "yes"), ("yes", SECRET)):
            with self.subTest(persistent=persistent, importing=importing):
                result = subprocess.run(
                    ["sh", "-c", script],
                    env={
                        **os.environ,
                        "INSPIRCD_PERSISTENT": persistent,
                        "INSPIRCD_PERSISTENT_IMPORT": importing,
                    },
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")
                self.assertNotIn(SECRET, result.stderr)
                self.assertEqual(list(self.destination.iterdir()), [])

    def test_entrypoint_materializes_token_before_opt_in_helper(self):
        script = (ROOT / "entrypoint.sh").read_text()
        self.assertLess(
            script.index(". /materialize-nocodb-token.sh"), script.index("INSPIRCD_ROOT=")
        )
        self.assertLess(script.index("INSPIRCD_ROOT="), script.index("/seed-persistent-config"))
        self.assertIn('if [ "${INSPIRCD_PERSISTENT:-no}" = yes ]; then', script)
        self.assertIn("/seed-persistent-config &", script)
        self.assertIn('wait "$_seed_pid"', script)
        self.assertIn('[ "$_seed_status" -eq 0 ] || exit 1', script)
        self.assertIn("elif [ ! -e $INSPIRCD_ROOT/conf/inspircd.conf ]", script)


if __name__ == "__main__":
    unittest.main()
