#!/usr/bin/env python3
"""Check generated API coverage, onboarding wiring and executable docs examples."""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class DocsContract(unittest.TestCase):
    def test_generated_sources(self):
        subprocess.run(
            ["python3", "tools/generate_docs_reference.py", "--check"],
            cwd=ROOT,
            check=True,
        )
        reference = (ROOT / "site-docs/reference/callable-api.md").read_text()
        for source in (
            "modules/js_api.d.ts",
            "modules/js_fetch.d.ts",
            "modules/js_generated/inspircd.d.ts",
        ):
            declaration = (ROOT / source).read_text().rstrip()
            self.assertIn(declaration, reference)
            for name in re.findall(r"^  ([A-Za-z][A-Za-z0-9]*)\(", declaration, re.M):
                self.assertIn("`" + name + "`", reference)

    def test_motd_installation(self):
        self.assertRegex((ROOT / "conf/inspircd.conf").read_text(), r'motd="docker.motd"')
        self.assertRegex((ROOT / "Dockerfile").read_text(), r"COPY (?:--[^ ]+ )*conf/\s+/conf")
        motd = (ROOT / "conf/docker.motd").read_text()
        self.assertIn("https://github.com/r33drichards/docker", motd)
        self.assertIn("/MOTD", motd)
        self.assertNotIn("https://r33drichards.github.io/docker", motd)

    def test_javascript_examples(self):
        for source in sorted((ROOT / "site-docs").rglob("*.md")):
            examples = re.findall(r"~~~javascript\n(.*?)\n~~~", source.read_text(), re.S)
            for index, example in enumerate(examples):
                with self.subTest(source=str(source.relative_to(ROOT)), index=index):
                    with tempfile.TemporaryDirectory() as folder:
                        path = Path(folder) / "example.js"
                        path.write_text(example)
                        subprocess.run(["node", "--check", str(path)], check=True)
                        if source.name == "replay.md":
                            subprocess.run(
                                ["node", "tests/replay/run.cjs", str(path)],
                                cwd=ROOT,
                                check=True,
                            )


if __name__ == "__main__":
    unittest.main()
