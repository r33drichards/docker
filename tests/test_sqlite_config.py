"""The persistent-directory emitter is opt-in and grants no scripts implicitly."""

import os
import pathlib
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class SQLiteConfigTests(unittest.TestCase):
    def run_config(self, enabled=None, nocodb="no"):
        env = dict(os.environ, INSPIRCD_NOCODB_WCHAT=nocodb)
        env.pop("INSPIRCD_SQLITE", None)
        if enabled is not None:
            env["INSPIRCD_SQLITE"] = enabled
        return subprocess.run(
            ["sh", str(ROOT / "conf/sqlite.sh")],
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_disabled(self):
        for enabled in (None, "no", ""):
            result = self.run_config(enabled)
            self.assertEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")

    def test_invalid(self):
        for enabled in ("true", 'yes"><module name="bad"'):
            result = self.run_config(enabled)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("<die ", result.stdout)
            self.assertNotIn("<javascript", result.stdout)

    def test_fixed_root_and_no_grants(self):
        for nocodb in ("no", "yes"):
            result = self.run_config("yes", nocodb)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('sqlitedirectory="/inspircd/data/script-databases"', result.stdout)
            self.assertNotIn("databases=", result.stdout)
            self.assertEqual(result.stdout.count('<module name="javascript">'), nocodb != "yes")

    def test_default_config_includes_emitter(self):
        self.assertIn(
            '<include executable="sh conf/sqlite.sh">',
            (ROOT / "conf/inspircd.conf").read_text(),
        )


class PersistentConfigTests(unittest.TestCase):
    def run_config(self, name, enabled):
        env = dict(os.environ, INSPIRCD_PERSISTENT=enabled)
        return subprocess.run(
            ["sh", str(ROOT / "conf" / name)], env=env, capture_output=True, text=True, check=False
        )

    def test_permanent_channel_module_without_automatic_modes(self):
        result = self.run_config("persistent.sh", "yes")
        self.assertEqual(result.returncode, 0)
        self.assertIn('<module name="permchannels">', result.stdout)
        self.assertIn('filename="/inspircd/data/xline.db"', result.stdout)
        self.assertIn('file="/inspircd/data/permchannels.conf" missingokay="yes"', result.stdout)
        self.assertNotIn("<permchannels channel=", result.stdout)
        self.assertIn('operonly="yes"', result.stdout)
        self.assertEqual(self.run_config("persistent.sh", "no").stdout, "")

    def test_bounded_logging_and_unchanged_opt_out(self):
        result = self.run_config("logging.sh", "yes")
        self.assertEqual(result.returncode, 0)
        self.assertIn('method="bounded"', result.stdout)
        self.assertIn('maxbytes="10485760" files="5"', result.stdout)
        self.assertIn('level="normal" type="* -USERINPUT -USEROUTPUT"', result.stdout)
        self.assertNotIn('method="file"', result.stdout)
        self.assertIn('method="file"', self.run_config("logging.sh", "no").stdout)
        for name in ("logging.sh", "persistent.sh"):
            result = self.run_config(name, "invalid")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("<die ", result.stdout)


if __name__ == "__main__":
    unittest.main()
