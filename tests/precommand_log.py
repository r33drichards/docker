#!/usr/bin/env python3
"""Check the unredacted repr using only synthetic command data."""

import argparse
import ast
import re
from collections import Counter
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("logfile", type=Path)
parser.add_argument("--expect-unloaded", action="store_true")
args = parser.parse_args()
text = args.logfile.read_text()
pattern = r'stage=OnPreCommand ParsedCommand\{command=("(?:[^"\\]|\\.)*"), parameters=(\[[^\n]*\])\} validated=true'
records = re.findall(pattern, text)
if args.expect_unloaded:
    assert not records and "stage=OnPostCommand" not in text, text
    print("PASS: pre-command logging is opt-in")
else:
    assert records, "No pre-command records found"
    counts = Counter()
    captured = []
    for command, params in records:
        # Repr is ASCII with fixed-width byte escapes, not JSON. Python's literal
        # parser decodes its quoted strings; Latin-1 recovers the original bytes.
        command = ast.literal_eval(command).encode("latin1")
        values = [value.encode("latin1") for value in ast.literal_eval(params)]
        counts[command] += 1
        captured.append((command, values))
    assert counts[b"PING"] == 6, counts
    expected = [
        (b"NICK", [b"hookcaller"]),
        (b"USER", [b"hookcaller", b"0", b"*", b"Hook test"]),
        (b"JOIN", [b"#hooktest"]),
        (b"JOIN", [b"#hooktest", b"channel-key-secret"]),
        (b"MODE", [b"#hooktest", b"+k", b"channel-key-secret"]),
        (b"PRIVMSG", [b"hookobserver", "private-message-secret two  spaces café".encode()]),
        (b"OPER", [b"operator-name-secret", b"operator-password-secret"]),
        (b"PASS", [b"connection-password-secret"]),
        (b"AUTHENTICATE", [b"sasl-payload-secret"]),
        (
            b"WEBIRC",
            [
                b"gateway-password-secret",
                b"gateway-user-secret",
                b"gateway-host-secret",
                b"192.0.2.1",
            ],
        ),
        (b"PRIVMSG", [b"NickServ", b"IDENTIFY service-password-secret"]),
        (b"VERSION", []),
        (b"PING", [b"tag-ping-secret"]),
    ]
    for record in expected:
        assert record in captured, (record, captured)
    assert b"UNKNOWNSECRET" not in counts and b"DIE" not in counts, counts
    hook_lines = [line for line in text.splitlines() if "stage=OnPreCommand" in line]
    assert len(hook_lines) == len(records), hook_lines
    assert all(all(0x20 <= ord(char) < 0x7F for char in line) for line in hook_lines), hook_lines
    assert "message-tag-secret" not in "\n".join(hook_lines), hook_lines
    assert "<redacted>" not in "\n".join(hook_lines), hook_lines
    all_hook_lines = [line for line in text.splitlines() if "ParsedCommand{" in line]
    assert not any("stage=OnUser" in line for line in text.splitlines()), text
    assert all(all(0x20 <= ord(char) < 0x7F for char in line) for line in all_hook_lines), (
        all_hook_lines
    )

    # Successful handlers emit a pre snapshot followed by a post snapshot.
    for fragment in [
        "private-message-secret",
        'ParsedCommand{command="JOIN", parameters=["#hooktest"]}',
        'ParsedCommand{command="VERSION", parameters=[]}',
    ]:
        matches = [line for line in all_hook_lines if fragment in line]
        assert len(matches) == 2, matches
        assert "stage=OnPreCommand" in matches[0], matches
        assert "stage=OnPostCommand" in matches[1] and "result=SUCCESS loop=false" in matches[1], (
            matches
        )

    # Validation can pass even when the handler then fails.
    for fragment in ["operator-password-secret", "IDENTIFY service-password-secret"]:
        matches = [line for line in all_hook_lines if fragment in line]
        assert len(matches) == 2 and "stage=OnPreCommand" in matches[0], matches
        assert "stage=OnPostCommand" in matches[1] and "result=FAILURE loop=false" in matches[1], (
            matches
        )
    assert not any("UNKNOWNSECRET" in line or 'command="DIE"' in line for line in all_hook_lines), (
        all_hook_lines
    )
    print("PASS: full snapshots, pre/post command order, and success/failure results")
