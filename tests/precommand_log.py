#!/usr/bin/env python3
"""Check structured pre-command records captured from server output."""

import argparse
import json
import re
from collections import Counter
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("logfile", type=Path)
parser.add_argument("--expect-unloaded", action="store_true")
args = parser.parse_args()
text = args.logfile.read_text()
pattern = r'command=([A-Z0-9]+) params=(\[[^\n]*\]) parameter_count=(\d+) validated=true'
records = re.findall(pattern, text)
if args.expect_unloaded:
    assert not records, records
    print("PASS: pre-command logging is opt-in")
else:
    assert records, "No pre-command records found"
    counts = Counter()
    shapes = set()
    for command, params, count in records:
        values = json.loads(params)
        assert values == ["<redacted>"] * int(count), (command, params, count)
        counts[command] += 1
        shapes.add((command, int(count)))
    assert counts["PING"] == 6, counts  # Six valid PINGs; one invalid PING is excluded.
    for shape in (("VERSION", 0), ("NICK", 1), ("USER", 4), ("JOIN", 1), ("JOIN", 2),
                  ("MODE", 3), ("PRIVMSG", 2), ("OPER", 2), ("PASS", 1),
                  ("AUTHENTICATE", 1), ("WEBIRC", 4)):
        assert shape in shapes, (shape, shapes)
    assert "UNKNOWNSECRET" not in counts and "DIE" not in counts, counts
    # Check hook records rather than unrelated logs from other server modules.
    hook_lines = "\n".join(line for line in text.splitlines() if "command=" in line)
    for private in ("secret", "hookcaller", "hookobserver", "#hooktest", "192.0.2.1", "café"):
        assert private not in hook_lines, (private, hook_lines)
    print("PASS: parsed shapes, one validated-stage record, and complete parameter redaction")
