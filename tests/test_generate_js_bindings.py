#!/usr/bin/env python3
"""Contract and drift tests for the V8-independent binding generator."""

from __future__ import annotations

import copy
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "tools/generate_js_bindings.py"
SPEC = importlib.util.spec_from_file_location("generate_js_bindings", GENERATOR)
assert SPEC is not None and SPEC.loader is not None
GEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GEN)
MANIFEST = ROOT / "bindings/inspircd.json"


class GeneratorTests(unittest.TestCase):
    def setUp(self):
        self.manifest = GEN.load_manifest(MANIFEST)

    def rejects(self, change, match=None):
        change(self.manifest)
        with self.assertRaisesRegex(GEN.ManifestError, match or ".+"):
            GEN.render_outputs(self.manifest)

    def cli(self, *args):
        return subprocess.run(
            [sys.executable, str(GENERATOR), *map(str, args)],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_exact_enabled_surface_and_adapters(self):
        outputs = GEN.render_outputs(self.manifest)
        rows = [
            line
            for line in outputs["inspircd_bindings.inc"].splitlines()
            if line.startswith("IRC_JS_BINDING(")
        ]
        self.assertEqual(
            rows,
            [
                'IRC_JS_BINDING("defaultIsNick", JsIrcdDefaultIsNick, 1)',
                'IRC_JS_BINDING("defaultIsUser", JsIrcdDefaultIsUser, 1)',
                'IRC_JS_BINDING("isChannel", JsIrcdIsChannel, 1)',
                'IRC_JS_BINDING("isFQDN", JsIrcdIsFQDN, 1)',
                'IRC_JS_BINDING("isHost", JsIrcdIsHost, 2)',
                'IRC_JS_BINDING("isNick", JsIrcdIsNick, 1)',
                'IRC_JS_BINDING("isSID", JsIrcdIsSID, 1)',
                'IRC_JS_BINDING("isUser", JsIrcdIsUser, 1)',
                'IRC_JS_BINDING("isValidMask", JsIrcdIsValidMask, 1)',
                'IRC_JS_BINDING("match", JsIrcdMatch, 2)',
                'IRC_JS_BINDING("matchCIDR", JsIrcdMatchCIDR, 2)',
                'IRC_JS_BINDING("matchMask", JsIrcdMatchMask, 3)',
                'IRC_JS_BINDING("processColors", JsIrcdProcessColors, 1)',
                'IRC_JS_BINDING("randomInt", JsIrcdRandomInt, 1)',
                'IRC_JS_BINDING("randomString", JsIrcdRandomString, 1)',
                'IRC_JS_BINDING("stripColor", JsIrcdStripColor, 1)',
                'IRC_JS_BINDING("time", JsIrcdTime, 0)',
                'IRC_JS_BINDING("timeNanoseconds", JsIrcdTimeNanoseconds, 0)',
                'IRC_JS_BINDING("timingSafeCompare", JsIrcdTimingSafeCompare, 2)',
            ],
        )
        declarations = outputs["inspircd.d.ts"]
        for signature in (
            "isChannel(value: string): boolean;",
            "isNick(value: string): boolean;",
            "match(str: string, pattern: string): boolean;",
            "matchCIDR(str: string, pattern: string): boolean;",
            "stripColor(value: string): string;",
            "time(): number;",
            "isHost(value: string, allowsimple: boolean): boolean;",
            "randomInt(maximum: number): number;",
            "randomString(length: number): string;",
            "timingSafeCompare(first: string, second: string): boolean;",
        ):
            self.assertIn(signature, declarations)

    def test_inventory_only_entries_never_generate_callbacks(self):
        outputs = GEN.render_outputs(self.manifest)
        inventory = json.loads(outputs["inspircd_inventory.json"])
        self.assertEqual(
            inventory["counts"],
            {
                "exposed": 19,
                "excluded": 23,
                "requires_manual_wrapper": 3,
                "planned_mutation": 9,
            },
        )
        for entry in inventory["inventory"]:
            self.assertFalse(entry["exposed"])
            if entry["classification"] == "planned_mutation":
                self.assertEqual(entry["authorization"], "pending_authority_decision")
                self.assertNotIn(entry["proposed_js_name"], outputs["inspircd_bindings.inc"])
                self.assertNotIn(entry["proposed_js_name"], outputs["inspircd.d.ts"])
        self.assertTrue(all(entry["exposed"] for entry in inventory["bindings"]))

    def test_deterministic_for_reordered_objects_and_inventory(self):
        expected = GEN.render_outputs(self.manifest)
        self.manifest["bindings"].reverse()
        self.manifest["inventory"].reverse()
        reordered = json.loads(json.dumps(self.manifest, sort_keys=True))
        self.assertEqual(expected, GEN.render_outputs(reordered))
        self.assertEqual(expected, GEN.render_outputs(self.manifest))

    def test_generated_files_are_current(self):
        for name, expected in GEN.render_outputs(self.manifest).items():
            self.assertEqual(
                (ROOT / "modules/js_generated" / name).read_bytes(), expected.encode("utf-8")
            )

    def test_duplicate_js_names_rejected(self):
        self.rejects(lambda d: d["bindings"].append(copy.deepcopy(d["bindings"][0])), "duplicate")

    def test_duplicate_adapter_symbols_rejected(self):
        self.rejects(
            lambda d: d["bindings"][1].update(adapter=d["bindings"][0]["adapter"]), "duplicate"
        )

    def test_duplicate_argument_names_rejected(self):
        self.rejects(lambda d: d["bindings"][0]["arguments"][1].update(name="str"), "duplicate")

    def test_duplicate_inventory_ids_rejected(self):
        self.rejects(lambda d: d["inventory"].append(copy.deepcopy(d["inventory"][0])), "duplicate")

    def test_proposed_names_cannot_shadow_enabled_api(self):
        def change(data):
            next(row for row in data["inventory"] if row["classification"] == "planned_mutation")[
                "proposed_js_name"
            ] = "time"

        self.rejects(change, "duplicate")

    def test_unknown_keys_rejected_at_every_schema_level(self):
        locations = [
            (),
            ("upstream",),
            ("policies",),
            ("bindings", 0),
            ("bindings", 0, "native"),
            ("bindings", 0, "arguments", 0),
            ("bindings", 0, "returns"),
            ("inventory", 0),
            ("inventory", 0, "native"),
        ]
        for location in locations:
            with self.subTest(location=location):
                data = copy.deepcopy(self.manifest)
                node = data
                for key in location:
                    node = node[key]
                node["unknown"] = True
                with self.assertRaisesRegex(GEN.ManifestError, "unknown keys"):
                    GEN.render_outputs(data)

    def test_mandatory_policy_fields_rejected_when_missing_or_empty(self):
        for policy in ("arguments", "errors", "lifetime", "authorization", "registration", "scope"):
            for missing in (True, False):
                with self.subTest(policy=policy, missing=missing):
                    data = copy.deepcopy(self.manifest)
                    if missing:
                        del data["policies"][policy]
                    else:
                        data["policies"][policy] = " "
                    with self.assertRaises(GEN.ManifestError):
                        GEN.render_outputs(data)
        for field in ("authorization", "lifetime", "errors", "side_effects"):
            data = copy.deepcopy(self.manifest)
            del data["bindings"][0][field]
            with self.assertRaisesRegex(GEN.ManifestError, "missing keys"):
                GEN.render_outputs(data)

    def test_unsupported_types_rejected(self):
        for kind in ("pointer", "User*", "array", "any", {}, None):
            with self.subTest(kind=kind):
                data = copy.deepcopy(self.manifest)
                data["bindings"][0]["arguments"][0]["type"] = kind
                with self.assertRaisesRegex(GEN.ManifestError, "supported types"):
                    GEN.render_outputs(data)
                data = copy.deepcopy(self.manifest)
                data["bindings"][0]["returns"]["type"] = kind
                with self.assertRaisesRegex(GEN.ManifestError, "supported types"):
                    GEN.render_outputs(data)

    def test_native_symbol_must_be_canonical(self):
        self.rejects(
            lambda d: d["bindings"][0]["native"].update(symbol="ServerInstance->Match"),
            "canonical C\\+\\+ symbol",
        )

    def test_reviewed_native_mapping_cannot_be_swapped(self):
        self.rejects(
            lambda d: d["bindings"][0]["native"].update(symbol="InspIRCd::Exit"),
            "canonical native contract",
        )

    def test_native_call_cannot_be_injected(self):
        self.rejects(
            lambda d: d["bindings"][0]["native"].update(call="InspIRCd::Exit(0)"),
            "canonical native contract",
        )

    def test_overload_cannot_be_swapped(self):
        self.rejects(
            lambda d: d["bindings"][0]["native"].update(
                declaration="static bool Match(const char*, const char*);"
            ),
            "canonical native contract",
        )

    def test_adapter_identifier_cannot_inject_cpp(self):
        self.rejects(
            lambda d: d["bindings"][0].update(adapter="JsIrcdMatch); dangerous(); ("),
            "canonical identifier",
        )

    def test_reserved_js_names_rejected(self):
        self.rejects(
            lambda d: d["bindings"][0].update(js_name="constructor"), "canonical identifier"
        )

    def test_unreviewed_binding_cannot_be_added(self):
        self.rejects(lambda d: d["bindings"][0].update(js_name="newFunction"), "not been audited")

    def test_header_path_cannot_traverse(self):
        self.rejects(
            lambda d: d["bindings"][0]["native"].update(header="include/../secret.h"),
            "canonical include",
        )

    def test_upstream_version_must_match_reviewed_contract(self):
        self.rejects(lambda d: d["upstream"].update(version="5.0.0"), "reviewed against")

    def test_schema_version_boolean_is_not_an_integer(self):
        self.rejects(lambda d: d.update(schema_version=True), "integer schema")

    def test_enabled_mutation_authority_cannot_be_implied(self):
        self.rejects(lambda d: d["bindings"][0].update(authorization="full_server"), "read_only")

    def test_pending_mutation_cannot_be_approved_in_inventory(self):
        def change(data):
            next(row for row in data["inventory"] if row["classification"] == "planned_mutation")[
                "authorization"
            ] = "approved"

        self.rejects(change, "must remain pending")

    def test_inventory_cannot_introduce_an_adapter(self):
        self.rejects(
            lambda d: d["inventory"][0].update(adapter="JsIrcdNewFunction"), "unknown keys"
        )

    def test_duplicate_json_keys_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "invalid.json"
            path.write_text('{"schema_version":1,"schema_version":1}', encoding="utf-8")
            with self.assertRaisesRegex(GEN.ManifestError, "duplicate key"):
                GEN.load_manifest(path)

    def test_comment_injection_rejected(self):
        self.rejects(
            lambda d: d["bindings"][0].update(description="*/ forged TypeScript"), "terminators"
        )

    def test_check_detects_missing_and_stale_without_writing(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "generated"
            result = self.cli("--output-dir", output, "--check")
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertFalse(output.exists())
            result = self.cli("--output-dir", output)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = self.cli("--output-dir", output, "--check")
            self.assertEqual(result.returncode, 0, result.stderr)
            target = output / "inspircd_bindings.inc"
            target.write_bytes(b"stale\n")
            result = self.cli("--output-dir", output, "--check")
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("inspircd_bindings.inc", result.stderr)
            self.assertEqual(target.read_bytes(), b"stale\n")
            target.unlink()
            result = self.cli("--output-dir", output, "--check")
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertFalse(target.exists())

    def test_invalid_manifest_leaves_no_partial_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / "invalid.json"
            output = Path(directory) / "generated"
            self.manifest["bindings"][0]["native"]["symbol"] = "InspIRCd::Exit"
            manifest.write_text(json.dumps(self.manifest), encoding="utf-8")
            result = self.cli("--manifest", manifest, "--output-dir", output)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertIn("canonical native contract", result.stderr)
            self.assertFalse(output.exists())

    def test_conversion_contracts_document_numeric_and_string_edges(self):
        bindings = {row["js_name"]: row for row in self.manifest["bindings"]}
        self.assertIn("9007199254740991", bindings["time"]["returns"]["conversion"])
        self.assertIn("before conversion", bindings["time"]["returns"]["conversion"])
        self.assertIn("value_copy", bindings["stripColor"]["returns"]["conversion"])
        for name in ("match", "matchCIDR"):
            for argument in bindings[name]["arguments"]:
                self.assertIn("embedded NUL", argument["conversion"])
                self.assertIn("TypeError", argument["conversion"])


if __name__ == "__main__":
    unittest.main()
