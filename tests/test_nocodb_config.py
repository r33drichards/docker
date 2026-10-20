"""Test the opt-in fixed-destination configuration emitter using fake secrets."""

import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent


class ConfigTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = pathlib.Path(self.temp.name)
        self.secret = root / "token"
        self.helper = root / "nocodb.sh"
        self.helper.write_text(
            (ROOT / "conf/nocodb.sh")
            .read_text()
            .replace("/inspircd/.secrets/nocodb-token", str(self.secret))
        )

    def run_config(self, enabled=None):
        env = dict(os.environ)
        env.pop("INSPIRCD_NOCODB_WCHAT", None)
        if enabled is not None:
            env["INSPIRCD_NOCODB_WCHAT"] = enabled
        result = subprocess.run(
            ["sh", str(self.helper)], env=env, capture_output=True, text=True, check=False
        )
        self.assertNotIn("synthetic-secret", result.stdout + result.stderr)
        return result

    def write_secret(self, value="synthetic-secret", mode=0o600):
        if self.secret.exists():
            self.secret.chmod(0o600)
        self.secret.write_text(value)
        self.secret.chmod(mode)

    def test_default_and_no_emit_nothing(self):
        for enabled in (None, "no", ""):
            result = self.run_config(enabled)
            self.assertEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")
            self.assertFalse(self.secret.exists())

    def test_invalid_activation_and_missing_secret_fail(self):
        for enabled in ("yes", "true", 'yes"><module name="bad"'):
            result = self.run_config(enabled)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("<die ", result.stdout)
            self.assertNotIn("<javascript", result.stdout)

    def test_valid_fixed_config(self):
        for mode in (0o400, 0o600):
            self.write_secret(mode=mode)
            result = self.run_config("yes")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.count('<module name="javascript">'), 1)
            self.assertIn(
                'url="https://nocodb-render-mvp.onrender.com/api/v2/tables/mdz86ygcx26cnd6/records"',
                result.stdout,
            )
            self.assertIn('endpoint="wchat_events" name="xc-token"', result.stdout)
            self.assertIn(
                'file="/inspircd/scripts/wchat-nocodb.js" authority="admin"', result.stdout
            )

    def test_unsafe_mode(self):
        self.write_secret(mode=0o644)
        self.assertNotEqual(self.run_config("yes").returncode, 0)

    def test_bad_size(self):
        for value in ("", "x" * 1025):
            self.write_secret(value)
            self.assertNotEqual(self.run_config("yes").returncode, 0)

    def test_symlink_and_directory(self):
        self.secret.symlink_to("missing")
        self.assertNotEqual(self.run_config("yes").returncode, 0)
        self.secret.unlink()
        self.secret.mkdir()
        self.assertNotEqual(self.run_config("yes").returncode, 0)


if __name__ == "__main__":
    unittest.main()
