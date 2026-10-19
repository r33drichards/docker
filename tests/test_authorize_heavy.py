#!/usr/bin/env python3
"""Positive/negative tests for the ordinary ci:heavy workload selector.

This selector is intentionally not a security boundary against edited PR code.
"""

import importlib.util
import io
import json
import os
import pathlib
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location(
    "authorize_heavy", pathlib.Path(__file__).parent / "ci" / "authorize_heavy.py"
)
assert SPEC is not None and SPEC.loader is not None
SELECTOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SELECTOR)


class HeavySelectorTests(unittest.TestCase):
    def invoke(self, *, labels=True, current_sha="head", events=None, permission="write"):
        paths = []
        if events is None:
            events = [
                {
                    "event": "labeled",
                    "label": {"name": "ci:heavy"},
                    "actor": {"login": "maintainer"},
                }
            ]

        def fetch(request, timeout):
            self.assertEqual(timeout, 20)
            self.assertEqual(
                request.get_header("Authorization"), "Bearer synthetic-read-only-token"
            )
            paths.append(request.full_url)
            if "/pulls/" in request.full_url:
                data = {
                    "head": {"sha": current_sha},
                    "labels": [{"name": "ci:heavy"}] if labels else [],
                }
            elif "/events?" in request.full_url:
                data = events
            elif "/collaborators/" in request.full_url:
                data = {"permission": permission}
            else:
                raise AssertionError(request.full_url)
            return io.BytesIO(json.dumps(data).encode())

        with tempfile.TemporaryDirectory() as directory:
            event = pathlib.Path(directory) / "event.json"
            output = pathlib.Path(directory) / "output"
            event.write_text(json.dumps({"pull_request": {"number": 3, "head": {"sha": "head"}}}))
            environment = {
                "GITHUB_EVENT_PATH": str(event),
                "GITHUB_REPOSITORY": "example/repository",
                "GH_TOKEN": "synthetic-read-only-token",
                "GITHUB_OUTPUT": str(output),
            }
            with (
                mock.patch.dict(os.environ, environment),
                mock.patch.object(SELECTOR.urllib.request, "urlopen", fetch),
            ):
                SELECTOR.main()
            return output.read_text(), paths

    def test_label_absent_does_not_request_heavy_work(self):
        output, paths = self.invoke(labels=False)
        self.assertEqual(output, "enabled=false\n")
        self.assertEqual(len(paths), 1)

    def test_stale_head_does_not_request_heavy_work(self):
        output, paths = self.invoke(current_sha="newer")
        self.assertEqual(output, "enabled=false\n")
        self.assertEqual(len(paths), 1)

    def test_maintainer_roles_enable_work(self):
        for role in ("admin", "maintain", "write"):
            with self.subTest(role=role):
                output, _ = self.invoke(permission=role)
                self.assertEqual(output, "enabled=true\n")

    def test_read_or_triage_does_not_authorize_work(self):
        for role in ("read", "triage", "none"):
            with self.subTest(role=role), self.assertRaises(RuntimeError):
                self.invoke(permission=role)

    def test_missing_label_history_fails_closed(self):
        with self.assertRaises(RuntimeError):
            self.invoke(events=[])

    def test_last_label_actor_is_checked(self):
        events = [
            {"event": "labeled", "label": {"name": "ci:heavy"}, "actor": {"login": "old"}},
            {"event": "unlabeled", "label": {"name": "ci:heavy"}, "actor": {"login": "old"}},
            {"event": "labeled", "label": {"name": "ci:heavy"}, "actor": {"login": "current"}},
        ]
        _, paths = self.invoke(events=events)
        self.assertTrue(paths[-1].endswith("/collaborators/current/permission"))

    def test_bounded_history_fails_closed(self):
        with self.assertRaises(RuntimeError):
            self.invoke(events=[{"event": "commented"}] * 100)

    def test_api_error_propagates(self):
        with mock.patch.object(
            SELECTOR.urllib.request, "urlopen", side_effect=OSError("API unavailable")
        ):
            with tempfile.TemporaryDirectory() as directory:
                event = pathlib.Path(directory) / "event"
                event.write_text(
                    json.dumps({"pull_request": {"number": 3, "head": {"sha": "head"}}})
                )
                with mock.patch.dict(
                    os.environ,
                    {
                        "GITHUB_EVENT_PATH": str(event),
                        "GITHUB_REPOSITORY": "a/b",
                        "GH_TOKEN": "synthetic",
                    },
                ):
                    with self.assertRaises(OSError):
                        SELECTOR.main()


if __name__ == "__main__":
    unittest.main()
