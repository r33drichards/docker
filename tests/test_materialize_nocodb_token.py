"""Exercise the source-only helper using fake tokens and an isolated fixed-root copy."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

HELPER = Path(__file__).resolve().parents[1] / "tools/materialize-nocodb-token.sh"
TOKEN = "fake-nocodb-token-only_123"


class MaterializerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / "inspircd"
        self.root.mkdir(mode=0o755)
        self.helper = Path(self.tmp.name) / "helper.sh"
        # Production has a fixed path, with no environment override. Only this
        # isolated test copy substitutes the root so tests need no root privileges.
        self.helper.write_text(
            HELPER.read_text().replace("_noco_root=/inspircd", f"_noco_root={self.root}")
        )
        self.secret = self.root / ".secrets/nocodb-token"

    def run_helper(self, value=TOKEN, *, trace=False):
        env = os.environ.copy()
        env.pop("INSPIRCD_NOCODB_TOKEN", None)
        if value is not None:
            env["INSPIRCD_NOCODB_TOKEN"] = value
        # A pre-exported scratch name must not leak the subsequently assigned token.
        env["_noco_token"] = "hostile-export-marker"
        cmd = ['. "$1" || exit 7; env']
        args = ["sh"] + (["-x"] if trace else []) + ["-c", *cmd, "test", str(self.helper)]
        result = subprocess.run(args, env=env, capture_output=True, check=False)
        if value:
            self.assertNotIn(value.encode(), result.stdout + result.stderr)
        self.assertNotIn(b"INSPIRCD_NOCODB_TOKEN=", result.stdout)
        return result

    def test_absent_is_noop(self):
        result = self.run_helper(None)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.secret.parent.exists())

    def test_valid_and_rotation(self):
        for value in (TOKEN, "rotated-fake-token", "a" * 1024):
            result = self.run_helper(value, trace=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(self.secret.read_text(), value)
            self.assertEqual(self.secret.stat().st_uid, os.getuid())
            self.assertEqual(self.secret.stat().st_mode & 0o7777, 0o600)
            self.assertEqual(self.secret.parent.stat().st_mode & 0o7777, 0o700)
            self.assertEqual(list(self.secret.parent.iterdir()), [self.secret])

    def test_invalid_values(self):
        for value in ("", "a" * 1025, "has space", "x\ny", "x\ry", "x\ty", "x\x7fy", "é"):
            with self.subTest(value=repr(value)):
                self.assertEqual(self.run_helper(value).returncode, 7)
                self.assertFalse(self.secret.parent.exists())

    def test_symlink_directory(self):
        target = Path(self.tmp.name) / "target"
        target.mkdir(mode=0o700)
        self.secret.parent.symlink_to(target, target_is_directory=True)
        self.assertEqual(self.run_helper().returncode, 7)
        self.assertEqual(list(target.iterdir()), [])

    def test_unsafe_directory(self):
        self.secret.parent.mkdir(mode=0o755)
        self.assertEqual(self.run_helper().returncode, 7)
        self.assertFalse(self.secret.exists())

    def test_symlink_file_does_not_clobber(self):
        self.secret.parent.mkdir(mode=0o700)
        target = Path(self.tmp.name) / "target"
        target.write_text("untouched")
        self.secret.symlink_to(target)
        self.assertEqual(self.run_helper().returncode, 7)
        self.assertEqual(target.read_text(), "untouched")

    def test_existing_directory_file(self):
        self.secret.parent.mkdir(mode=0o700)
        self.secret.mkdir(mode=0o700)
        self.assertEqual(self.run_helper().returncode, 7)
        self.assertTrue(self.secret.is_dir())

    def test_hardlink_file(self):
        self.secret.parent.mkdir(mode=0o700)
        self.secret.write_text("untouched")
        self.secret.chmod(0o600)
        target = Path(self.tmp.name) / "target"
        target.hardlink_to(self.secret)
        self.assertEqual(self.run_helper().returncode, 7)
        self.assertEqual(target.read_text(), "untouched")

    def test_insecure_file_mode(self):
        self.secret.parent.mkdir(mode=0o700)
        self.secret.write_text("untouched")
        self.secret.chmod(0o644)
        self.assertEqual(self.run_helper().returncode, 7)
        self.assertEqual(self.secret.read_text(), "untouched")

    def test_unsafe_root(self):
        self.root.chmod(0o777)
        self.assertEqual(self.run_helper().returncode, 7)
        self.assertFalse(self.secret.parent.exists())

    def test_symlink_root(self):
        target = Path(self.tmp.name) / "actual-root"
        self.root.rename(target)
        self.root.symlink_to(target, target_is_directory=True)
        self.assertEqual(self.run_helper().returncode, 7)
        self.assertFalse(self.secret.parent.exists())

    def test_failed_rotation_preserves_previous(self):
        self.assertEqual(self.run_helper().returncode, 0)
        self.assertEqual(self.run_helper("bad\nvalue").returncode, 7)
        self.assertEqual(self.secret.read_text(), TOKEN)


if __name__ == "__main__":
    unittest.main()
