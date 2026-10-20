"""Compile and exercise the fixed-path bootstrap in an isolated filesystem tree."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tools/sqlite-bootstrap.c"


class BootstrapTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.fixture = Path(cls.tmp.name)
        cls.fixture.chmod(0o755)
        cls.uid = 10000 if os.geteuid() == 0 else os.getuid()
        cls.gid = 10000 if os.geteuid() == 0 else os.getgid()
        cls.entrypoint = cls.fixture / "entrypoint"
        helper = cls.fixture / "entrypoint.c"
        helper.write_text(
            "#define _GNU_SOURCE\n"
            "#include <stdio.h>\n#include <stdlib.h>\n#include <unistd.h>\n"
            "int main(int argc, char **argv) {\n"
            ' printf("%lu %lu %lu %lu %d %d\\n", (unsigned long)getuid(),\n'
            " (unsigned long)geteuid(), (unsigned long)getgid(),\n"
            " (unsigned long)getegid(), getgroups(0, NULL), setuid(0));\n"
            ' for (int i = 1; i < argc; ++i) printf("arg:%s\\n", argv[i]);\n'
            ' printf("token:%s\\n", getenv("INSPIRCD_NOCODB_TOKEN") ? "present" : "absent");\n'
            " return 0;\n}\n"
        )
        cls.compile(helper, cls.entrypoint)
        # Production has no path or identity overrides. Substitute literals only
        # in this private compiled test copy, as in the token materializer tests.
        source = SOURCE.read_text()
        source = source.replace('open("/",', f'open("{cls.fixture}/",')
        source = source.replace('"/entrypoint.sh"', f'"{cls.entrypoint}"')
        source = source.replace(
            '"/inspircd/persistent/%s"', f'"{cls.fixture}/inspircd/persistent/%s"'
        )
        source = source.replace("daemon_uid = 10000", f"daemon_uid = {cls.uid}")
        source = source.replace("daemon_gid = 10000", f"daemon_gid = {cls.gid}")
        cls.test_source = cls.fixture / "bootstrap.c"
        cls.test_source.write_text(source)
        cls.binary = cls.fixture / "bootstrap"
        cls.compile(cls.test_source, cls.binary, static=True)

    @classmethod
    def compile(cls, source, output, *, static=False):
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-O2",
                *(["-static"] if static else []),
                str(source),
                "-o",
                str(output),
            ],
            check=True,
            capture_output=True,
        )

    def setUp(self):
        self.root = self.fixture / "inspircd"
        self.root.mkdir(mode=0o755)
        if os.geteuid() == 0:
            os.chown(self.root, self.uid, self.gid)
        self.volume = self.root / "persistent"
        self.volume.mkdir(mode=0o755)
        self.names = ("conf", "data", "logs")
        self.directories = [self.volume / name for name in self.names]
        self.db = self.volume / "data/script-databases"
        self.private = [*self.directories, self.db]
        self.aliases = [self.root / name for name in self.names]
        for alias in self.aliases:
            alias.mkdir(mode=0o755)
        self.addCleanup(self.cleanup_root)

    def cleanup_root(self):
        if self.root.is_symlink():
            self.root.unlink()
        elif self.root.exists():
            shutil.rmtree(self.root)
        for name in ("moved-root", "outside"):
            path = self.fixture / name
            if path.exists():
                shutil.rmtree(path)
        self.fixture.chmod(0o755)

    def run_bootstrap(
        self, setting="yes", *, as_daemon=False, extra_env=None, mixed_identity=None, binary=None
    ):
        env = os.environ.copy()
        env.pop("INSPIRCD_PERSISTENT", None)
        env.pop("INSPIRCD_NOCODB_TOKEN", None)
        if setting is not None:
            env["INSPIRCD_PERSISTENT"] = setting
        if extra_env:
            env.update(extra_env)

        def drop_privileges():
            os.setgroups([])
            if mixed_identity == "real-root-uid":
                os.setresuid(0, self.uid, 0)
            elif mixed_identity == "effective-root-uid":
                os.setresuid(self.uid, 0, 0)
            elif mixed_identity == "real-root-gid":
                os.setresgid(0, self.gid, 0)
            elif mixed_identity == "effective-root-gid":
                os.setresgid(self.gid, 0, 0)
            else:
                os.setgid(self.gid)
                os.setuid(self.uid)

        return subprocess.run(
            [str(binary or self.binary), "--nofork", "two words"],
            env=env,
            preexec_fn=(
                drop_privileges if (as_daemon or mixed_identity) and os.geteuid() == 0 else None
            ),
            capture_output=True,
            text=True,
            check=False,
        )

    def ready_volume(self):
        for directory in self.private:
            directory.mkdir(mode=0o700)
            if os.geteuid() == 0:
                os.chown(directory, self.uid, self.gid)

    def assert_started(self, result):
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        identity = [int(value) for value in lines[0].split()]
        self.assertEqual(identity[:4], [self.uid, self.uid, self.gid, self.gid])
        self.assertEqual(identity[5], -1, "the entrypoint must not regain root")
        if os.geteuid() == 0:
            self.assertEqual(identity[4], 0, "root must clear supplementary groups")
        self.assertEqual(lines[1:3], ["arg:--nofork", "arg:two words"])

    def assert_refused(self, result):
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "", "the entrypoint must not run on failure")

    def assert_aliases(self):
        for alias, target in zip(self.aliases, self.directories, strict=True):
            self.assertTrue(alias.is_symlink())
            self.assertEqual(os.readlink(alias), str(target))

    def test_default_and_explicit_no_have_no_volume_side_effects(self):
        self.volume.rmdir()
        for setting in (None, "", "no"):
            with self.subTest(setting=setting):
                self.assert_started(
                    self.run_bootstrap(setting, extra_env={"INSPIRCD_SQLITE": "yes"})
                )
                self.assertFalse(self.volume.exists())
                self.assertTrue(
                    all(alias.is_dir() and not alias.is_symlink() for alias in self.aliases)
                )

    def test_invalid_flag_fails_closed_without_echoing_value(self):
        for setting in ("YES", "true", "1", "yes\n", "invalid-secret-marker"):
            with self.subTest(setting=setting):
                result = self.run_bootstrap(setting)
                self.assert_refused(result)
                self.assertNotIn("invalid-secret-marker", result.stderr)
                self.assertEqual(list(self.volume.iterdir()), [])

    def test_nonroot_validates_volume_and_converts_empty_image_aliases(self):
        self.ready_volume()
        before = [path.stat() for path in self.private]
        self.assert_started(self.run_bootstrap(as_daemon=True))
        self.assert_aliases()
        for old, path in zip(before, self.private, strict=True):
            current = path.stat()
            self.assertEqual(
                (old.st_uid, old.st_gid, old.st_mode, old.st_ctime_ns),
                (current.st_uid, current.st_gid, current.st_mode, current.st_ctime_ns),
            )

    def test_nonroot_does_not_initialize_missing_directories(self):
        self.assert_refused(self.run_bootstrap(as_daemon=True))
        self.assertEqual(list(self.volume.iterdir()), [])

    def test_missing_mount_is_not_created(self):
        self.volume.rmdir()
        self.assert_refused(self.run_bootstrap())
        self.assertFalse(self.volume.exists())

    def test_existing_correct_aliases_are_idempotent(self):
        self.ready_volume()
        for alias, target in zip(self.aliases, self.directories, strict=True):
            alias.rmdir()
            alias.symlink_to(target, target_is_directory=True)
        before = [alias.lstat() for alias in self.aliases]
        self.assert_started(self.run_bootstrap(as_daemon=True))
        self.assert_aliases()
        self.assertEqual(
            [old.st_ino for old in before], [alias.lstat().st_ino for alias in self.aliases]
        )

    def test_wrong_alias_target_is_not_changed(self):
        self.ready_volume()
        outside = self.fixture / "outside"
        outside.mkdir(mode=0o755)
        self.aliases[0].rmdir()
        self.aliases[0].symlink_to(outside, target_is_directory=True)
        self.assert_refused(self.run_bootstrap())
        self.assertEqual(os.readlink(self.aliases[0]), str(outside))
        self.assertEqual(list(outside.iterdir()), [])

    def test_nonempty_image_directories_are_never_migrated_or_deleted(self):
        self.ready_volume()
        for alias in self.aliases:
            with self.subTest(alias=alias.name):
                marker = alias / "keep-existing-state"
                marker.write_text("unchanged")
                self.assert_refused(self.run_bootstrap())
                self.assertFalse(alias.is_symlink())
                self.assertEqual(marker.read_text(), "unchanged")
                marker.unlink()

    def test_missing_alias_after_interrupted_conversion_is_recovered(self):
        self.ready_volume()
        self.aliases[0].rmdir()
        self.assert_started(self.run_bootstrap(as_daemon=True))
        self.assert_aliases()

    def test_regular_file_alias_is_not_deleted(self):
        self.ready_volume()
        self.aliases[0].rmdir()
        self.aliases[0].write_text("unchanged")
        self.assert_refused(self.run_bootstrap())
        self.assertEqual(self.aliases[0].read_text(), "unchanged")

    def test_unsafe_alias_permissions_are_not_repaired(self):
        self.ready_volume()
        self.aliases[0].chmod(0o777)
        self.assert_refused(self.run_bootstrap())
        self.assertFalse(self.aliases[0].is_symlink())
        self.assertEqual(self.aliases[0].stat().st_mode & 0o7777, 0o777)

    def test_symlink_child_does_not_change_target(self):
        self.ready_volume()
        outside = self.fixture / "outside"
        outside.mkdir(mode=0o700)
        if os.geteuid() == 0:
            os.chown(outside, self.uid, self.gid)
        self.db.rmdir()
        self.db.symlink_to(outside, target_is_directory=True)
        before = outside.stat()
        self.assert_refused(self.run_bootstrap())
        self.assertEqual(outside.stat().st_mode, before.st_mode)
        self.assertEqual(outside.stat().st_uid, before.st_uid)

    def test_child_regular_file_is_rejected(self):
        self.ready_volume()
        self.db.rmdir()
        self.db.write_text("unchanged")
        self.db.chmod(0o700)
        self.assert_refused(self.run_bootstrap())
        self.assertEqual(self.db.read_text(), "unchanged")

    def test_symlink_mount_is_rejected(self):
        self.ready_volume()
        outside = self.fixture / "outside"
        self.volume.rename(outside)
        self.volume.symlink_to(outside, target_is_directory=True)
        self.assert_refused(self.run_bootstrap())
        self.assertEqual(sorted(path.name for path in outside.iterdir()), ["conf", "data", "logs"])

    def test_symlink_inspircd_is_rejected(self):
        self.ready_volume()
        moved = self.fixture / "moved-root"
        self.root.rename(moved)
        self.root.symlink_to(moved, target_is_directory=True)
        self.assert_refused(self.run_bootstrap())
        self.assertTrue(self.db.exists())

    def test_unsafe_parent_modes_are_rejected_without_changes(self):
        self.ready_volume()
        for parent in (self.fixture, self.root, self.volume):
            for mode in (0o775, 0o777):
                with self.subTest(parent=parent.name, mode=oct(mode)):
                    parent.chmod(mode)
                    self.assert_refused(self.run_bootstrap())
                    self.assertEqual(parent.stat().st_mode & 0o7777, mode)
                    parent.chmod(0o755)

    def test_unsafe_private_directory_modes_are_not_repaired(self):
        self.ready_volume()
        for directory in self.private:
            for mode in (0o755, 0o770, 0o777, 0o1700):
                with self.subTest(directory=str(directory), mode=oct(mode)):
                    directory.chmod(mode)
                    self.assert_refused(self.run_bootstrap())
                    self.assertEqual(directory.stat().st_mode & 0o7777, mode)
                    directory.chmod(0o700)

    def test_bootstrap_preserves_token_for_existing_entrypoint_materializer(self):
        result = self.run_bootstrap(None, extra_env={"INSPIRCD_NOCODB_TOKEN": "fake-test-token"})
        self.assert_started(result)
        self.assertIn("token:present", result.stdout)
        self.assertNotIn("fake-test-token", result.stdout + result.stderr)

    def test_mixed_saved_or_real_ids_fail_closed_with_injected_identity_reads(self):
        # Actual mixed-ID launches are covered below in root CI. These compiled
        # syscall-read substitutions exercise every check even on a nonroot host.
        for getter, c_type, identity in (
            ("getresuid", "uid_t", self.uid),
            ("getresgid", "gid_t", self.gid),
        ):
            for field in ("real", "saved"):
                with self.subTest(getter=getter, field=field):
                    wrapper = self.fixture / "mixed-identity.c"
                    binary = self.fixture / "mixed-identity"
                    wrapper.write_text(
                        "#define _GNU_SOURCE\n#include <unistd.h>\n"
                        f"int injected({c_type} *real, {c_type} *effective, {c_type} *saved) {{\n"
                        f" *real = *effective = *saved = {identity}; *{field} = 0; return 0;\n"
                        "}\n"
                        f"#define {getter} injected\n"
                        f'#include "{self.test_source}"\n'
                    )
                    self.compile(wrapper, binary, static=True)
                    for setting in ("no", "yes"):
                        result = self.run_bootstrap(setting, binary=binary)
                        self.assert_refused(result)
                        self.assertIn("mixed or unreadable process identities", result.stderr)
                        self.assertEqual(list(self.volume.iterdir()), [])

    @unittest.skipUnless(os.geteuid() == 0, "requires root with UID/GID10000 available")
    def test_root_initializes_only_private_directories_and_drops_all_ids(self):
        mount = self.volume.stat()
        self.assert_started(self.run_bootstrap())
        self.assert_aliases()
        for directory in self.private:
            info = directory.stat()
            self.assertEqual(
                (info.st_uid, info.st_gid, info.st_mode & 0o7777), (self.uid, self.gid, 0o700)
            )
        self.assertEqual(
            (self.volume.stat().st_uid, self.volume.stat().st_gid, self.volume.stat().st_mode),
            (mount.st_uid, mount.st_gid, mount.st_mode),
        )
        self.assert_started(self.run_bootstrap())

    @unittest.skipUnless(os.geteuid() == 0, "requires root with UID/GID10000 available")
    def test_root_only_changes_fixed_directories_not_their_contents(self):
        markers = []
        for directory in self.private:
            directory.mkdir(mode=0o700)
            marker = directory / "do-not-chown"
            marker.write_text("unchanged")
            marker.chmod(0o600)
            markers.append(marker)
        self.assert_started(self.run_bootstrap())
        for marker in markers:
            self.assertEqual((marker.stat().st_uid, marker.stat().st_gid), (0, 0))
            self.assertEqual(marker.read_text(), "unchanged")

    @unittest.skipUnless(os.geteuid() == 0, "requires root with UID/GID10000 available")
    def test_unrelated_owner_is_rejected(self):
        self.ready_volume()
        for target in (self.root, self.volume, *self.private, *self.aliases):
            with self.subTest(target=str(target)):
                original = target.stat()
                os.chown(target, 12345, 12345)
                self.assert_refused(self.run_bootstrap())
                self.assertEqual(target.stat().st_uid, 12345)
                os.chown(target, original.st_uid, original.st_gid)

    @unittest.skipUnless(os.geteuid() == 0, "requires root with UID/GID10000 available")
    def test_incorrect_existing_daemon_group_is_rejected(self):
        self.ready_volume()
        for directory in self.private:
            with self.subTest(directory=str(directory)):
                os.chown(directory, self.uid, 12345)
                self.assert_refused(self.run_bootstrap())
                self.assertEqual(directory.stat().st_gid, 12345)
                os.chown(directory, self.uid, self.gid)

    @unittest.skipUnless(os.geteuid() == 0, "requires root with UID/GID10000 available")
    def test_mixed_identities_are_rejected_before_volume_or_entrypoint_access(self):
        for identity in (
            "real-root-uid",
            "effective-root-uid",
            "real-root-gid",
            "effective-root-gid",
        ):
            for setting in ("no", "yes"):
                with self.subTest(identity=identity, setting=setting):
                    result = self.run_bootstrap(setting, mixed_identity=identity)
                    self.assert_refused(result)
                    self.assertIn("mixed or unreadable process identities", result.stderr)
                    self.assertEqual(list(self.volume.iterdir()), [])

    @unittest.skipIf(os.geteuid() == 0, "exercises unprivileged user namespace root")
    def test_root_cannot_start_helpers_when_privilege_drop_is_denied(self):
        if shutil.which("unshare") is None:
            self.skipTest("unshare is not installed")
        probe = subprocess.run(["unshare", "-Ur", "true"], capture_output=True, check=False)
        if probe.returncode != 0:
            self.skipTest("unprivileged user namespaces are unavailable")
        # This one-ID namespace has real euid0 but forbids setgroups and has no
        # mapped daemon UID. Root must fail closed rather than run the entrypoint.
        env = os.environ.copy()
        env["INSPIRCD_PERSISTENT"] = "no"
        env.pop("INSPIRCD_NOCODB_TOKEN", None)
        result = subprocess.run(
            ["unshare", "-Ur", str(self.binary)],
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assert_refused(result)
        self.assertIn("cannot drop root privileges", result.stderr)
        self.assertEqual(list(self.volume.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
