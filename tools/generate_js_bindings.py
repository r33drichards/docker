#!/usr/bin/env python3
"""Generate the audited, primitive-only InspIRCd JavaScript binding surface.

Run from any directory:
    python3 tools/generate_js_bindings.py
    python3 tools/generate_js_bindings.py --check

The manifest is an allowlist, not a C++ parser or automatic object binder. Native
callbacks are handwritten by the embedding module. Adding a supported mapping
also requires extending CANONICAL_BINDINGS after reviewing its native contract.
Inventory-only entries can never generate a registration row or a TS method.
This tool uses only the Python standard library and does not require V8.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
OUTPUT_NAMES = (
    "inspircd_bindings.inc",
    "inspircd.d.ts",
    "inspircd_inventory.json",
)
IDENTIFIER = re.compile(r"[A-Za-z][A-Za-z0-9_]*\Z")
CANONICAL_SYMBOL = re.compile(r"[A-Za-z][A-Za-z0-9_]*(?:::[A-Za-z][A-Za-z0-9_]*)+\Z")
HEADER = re.compile(r"include/[a-z][a-z0-9_]*\.h\Z")
RESERVED_JS_NAMES = frozenset({"__proto__", "prototype", "constructor"})
SUPPORTED_TYPES = frozenset({"string", "boolean", "number"})
INVENTORY_SIDE_EFFECTS = frozenset(
    {
        "pure",
        "read_server_state",
        "randomness",
        "mutates_argument",
        "process_control",
        "network_configuration",
        "authentication",
        "borrowed_mutable_state",
        "mutates_server",
        "module_control",
        "sensitive_state",
    }
)
# Each record specifies the exact overload or callable hook. This is a reviewed
# contract catalogue, intentionally separate from the editable manifest.
CANONICAL_BINDINGS = {
    "match": (
        "JsIrcdMatch",
        "InspIRCd::Match",
        "include/inspircd.h",
        "static bool Match(const std::string& str, const std::string& pattern, const unsigned char* map = nullptr);",
        "InspIRCd::Match(str, pattern)",
        ("str", "pattern"),
        "boolean",
        "read_server_state",
    ),
    "matchCIDR": (
        "JsIrcdMatchCIDR",
        "InspIRCd::MatchCIDR",
        "include/inspircd.h",
        "static bool MatchCIDR(const std::string& str, const std::string& pattern, const unsigned char* map = nullptr);",
        "InspIRCd::MatchCIDR(str, pattern)",
        ("str", "pattern"),
        "boolean",
        "read_server_state",
    ),
    "isNick": (
        "JsIrcdIsNick",
        "InspIRCd::IsNick",
        "include/inspircd.h",
        "std::function<bool(const std::string_view&)> IsNick = &DefaultIsNick;",
        "ServerInstance->IsNick(value)",
        ("value",),
        "boolean",
        "read_server_state",
    ),
    "isChannel": (
        "JsIrcdIsChannel",
        "ChannelManager::IsChannel",
        "include/channelmanager.h",
        "std::function<bool(const std::string_view&)> IsChannel = DefaultIsChannel;",
        "ServerInstance->Channels.IsChannel(value)",
        ("value",),
        "boolean",
        "read_server_state",
    ),
    "stripColor": (
        "JsIrcdStripColor",
        "InspIRCd::StripColor",
        "include/inspircd.h",
        "static void StripColor(std::string& str);",
        "InspIRCd::StripColor(value_copy)",
        ("value",),
        "string",
        "copy_only",
    ),
    "time": (
        "JsIrcdTime",
        "InspIRCd::Time",
        "include/inspircd.h",
        "inline auto Time() const { return ts.tv_sec; }",
        "ServerInstance->Time()",
        (),
        "number",
        "read_server_state",
    ),
}


class ManifestError(ValueError):
    """An invalid or unaudited manifest cannot produce bindings."""


def fail(path: str, message: str) -> None:
    raise ManifestError(f"{path}: {message}")


def object_keys(
    value: Any, required: set[str], path: str, optional: frozenset[str] = frozenset()
) -> None:
    if not isinstance(value, dict):
        fail(path, "expected an object")
    missing = required - value.keys()
    unknown = value.keys() - required - optional
    if missing:
        fail(path, f"missing keys: {', '.join(sorted(missing))}")
    if unknown:
        fail(path, f"unknown keys: {', '.join(sorted(unknown))}")


def nonempty_text(value: Any, path: str) -> None:
    if not isinstance(value, str) or not value.strip():
        fail(path, "expected nonempty text")
    if any(ord(char) < 32 for char in value):
        fail(path, "control characters are not allowed")
    # All manifest prose is emitted as JSON or line comments. Block-comment
    # terminators would escape a generated declaration's documentation.
    if "*/" in value:
        fail(path, "block-comment terminators are not allowed")


def identifier(value: Any, path: str) -> None:
    nonempty_text(value, path)
    if not IDENTIFIER.fullmatch(value) or value in RESERVED_JS_NAMES:
        fail(path, "expected a safe canonical identifier")


def native_record(value: Any, path: str, enabled: bool) -> None:
    keys = {"symbol", "declaration", "header"}
    if enabled:
        keys.add("call")
    object_keys(value, keys, path)
    for key in keys:
        nonempty_text(value[key], f"{path}.{key}")
    if not CANONICAL_SYMBOL.fullmatch(value["symbol"]):
        fail(f"{path}.symbol", "expected a fully qualified canonical C++ symbol")
    if not HEADER.fullmatch(value["header"]):
        fail(f"{path}.header", "expected a canonical include/<header>.h path")


def require_list(value: Any, path: str) -> None:
    if not isinstance(value, list):
        fail(path, "expected an array")


def unique(value: str, seen: set[str], path: str) -> None:
    if value in seen:
        fail(path, f"duplicate value: {value}")
    seen.add(value)


def primitive_type(value: Any, path: str) -> None:
    if not isinstance(value, str) or value not in SUPPORTED_TYPES:
        fail(path, "supported types are string, boolean, and number")


def validate_manifest(data: Any) -> dict[str, Any]:
    object_keys(
        data,
        {"schema_version", "api_namespace", "upstream", "policies", "bindings", "inventory"},
        "manifest",
    )
    if type(data["schema_version"]) is not int or data["schema_version"] != 1:
        fail("schema_version", "only integer schema version 1 is supported")
    if data["api_namespace"] != "ircd":
        fail("api_namespace", "only the ircd namespace is supported")
    object_keys(data["upstream"], {"project", "version", "source"}, "upstream")
    expected_upstream = {
        "project": "InspIRCd",
        "version": "4.12.1",
        "source": "https://github.com/inspircd/inspircd/tree/v4.12.1/include",
    }
    if data["upstream"] != expected_upstream:
        fail("upstream", "canonical contracts were reviewed against InspIRCd v4.12.1")
    policy_keys = {"arguments", "errors", "lifetime", "authorization", "registration", "scope"}
    object_keys(data["policies"], policy_keys, "policies")
    for name in policy_keys:
        nonempty_text(data["policies"][name], f"policies.{name}")
    require_list(data["bindings"], "bindings")
    require_list(data["inventory"], "inventory")
    names: set[str] = set()
    adapters: set[str] = set()
    for index, binding in enumerate(data["bindings"]):
        path = f"bindings[{index}]"
        object_keys(
            binding,
            {
                "js_name",
                "adapter",
                "native",
                "arguments",
                "returns",
                "description",
                "side_effects",
                "authorization",
                "lifetime",
                "errors",
            },
            path,
        )
        for key in ("js_name", "adapter"):
            identifier(binding[key], f"{path}.{key}")
        unique(binding["js_name"], names, f"{path}.js_name")
        unique(binding["adapter"], adapters, f"{path}.adapter")
        native_record(binding["native"], f"{path}.native", True)
        require_list(binding["arguments"], f"{path}.arguments")
        argnames: set[str] = set()
        for argindex, argument in enumerate(binding["arguments"]):
            argpath = f"{path}.arguments[{argindex}]"
            object_keys(argument, {"name", "type", "conversion"}, argpath)
            identifier(argument["name"], f"{argpath}.name")
            unique(argument["name"], argnames, f"{argpath}.name")
            primitive_type(argument["type"], f"{argpath}.type")
            nonempty_text(argument["conversion"], f"{argpath}.conversion")
        object_keys(binding["returns"], {"type", "conversion"}, f"{path}.returns")
        primitive_type(binding["returns"]["type"], f"{path}.returns.type")
        nonempty_text(binding["returns"]["conversion"], f"{path}.returns.conversion")
        nonempty_text(binding["description"], f"{path}.description")
        for key, required in {
            "authorization": "read_only",
            "lifetime": "owned_values_call_only",
            "errors": "strict_arguments_native_exception_translation",
        }.items():
            if binding[key] != required:
                fail(f"{path}.{key}", f"enabled utility bindings require {required}")
        expected = CANONICAL_BINDINGS.get(binding["js_name"])
        if expected is None:
            fail(f"{path}.js_name", "native mapping has not been audited in CANONICAL_BINDINGS")
        adapter, symbol, header, declaration, call, args, result, effects = expected
        actual = (
            binding["adapter"],
            binding["native"]["symbol"],
            binding["native"]["header"],
            binding["native"]["declaration"],
            binding["native"]["call"],
            tuple(argument["name"] for argument in binding["arguments"]),
            binding["returns"]["type"],
            binding["side_effects"],
        )
        if actual != (adapter, symbol, header, declaration, call, args, result, effects):
            fail(path, "mapping differs from the reviewed canonical native contract")
        if any(argument["type"] != "string" for argument in binding["arguments"]):
            fail(f"{path}.arguments", "these reviewed adapters accept only strings")
    ids: set[str] = set()
    for index, entry in enumerate(data["inventory"]):
        path = f"inventory[{index}]"
        object_keys(
            entry,
            {
                "id",
                "native",
                "classification",
                "reason",
                "conversion",
                "side_effects",
                "authorization",
                "lifetime",
            },
            path,
            frozenset({"proposed_js_name"}),
        )
        identifier(entry["id"], f"{path}.id")
        unique(entry["id"], ids, f"{path}.id")
        native_record(entry["native"], f"{path}.native", False)
        for key in ("reason", "conversion", "lifetime", "side_effects"):
            nonempty_text(entry[key], f"{path}.{key}")
        if entry["side_effects"] not in INVENTORY_SIDE_EFFECTS:
            fail(f"{path}.side_effects", "unsupported side-effect policy")
        classification = entry["classification"]
        if classification not in ("excluded", "requires_manual_wrapper", "planned_mutation"):
            fail(f"{path}.classification", "unsupported inventory classification")
        if classification == "planned_mutation":
            if (
                entry["authorization"] != "pending_authority_decision"
                or entry["side_effects"] != "mutates_server"
            ):
                fail(path, "planned mutations must remain pending and identify server mutation")
            if "proposed_js_name" not in entry:
                fail(path, "planned mutations require a proposed_js_name")
            identifier(entry["proposed_js_name"], f"{path}.proposed_js_name")
            unique(entry["proposed_js_name"], names, f"{path}.proposed_js_name")
        elif entry["authorization"] != "not_exposed" or "proposed_js_name" in entry:
            fail(path, "excluded/manual entries must remain unexposed without a proposed JS name")
    return data


def _unique_json_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ManifestError(f"JSON object contains duplicate key: {key}")
        result[key] = value
    return result


def load_manifest(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as handle:
        return validate_manifest(json.load(handle, object_pairs_hook=_unique_json_object))


def render_outputs(data: dict[str, Any]) -> dict[str, str]:
    validate_manifest(data)
    bindings = sorted(data["bindings"], key=lambda row: row["js_name"])
    inventory = sorted(data["inventory"], key=lambda row: row["id"])
    normalized = dict(data, bindings=bindings, inventory=inventory)
    canonical_json = json.dumps(
        normalized, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    )
    digest = hashlib.sha256(canonical_json.encode("ascii")).hexdigest()
    banner = [
        "// Generated by tools/generate_js_bindings.py; do not edit.",
        "// Source: bindings/inspircd.json (InspIRCd v4.12.1).",
        f"// Manifest SHA-256: {digest}",
    ]
    registration = banner + [
        "// Define IRC_JS_BINDING(js_name, native_adapter, exact_arity) before inclusion.",
        "// This is an X-macro inventory, not callback implementations or authorization.",
        "#ifndef IRC_JS_BINDING",
        '# error "Define IRC_JS_BINDING before including inspircd_bindings.inc"',
        "#endif",
    ]
    for binding in bindings:
        registration.append(
            f'IRC_JS_BINDING("{binding["js_name"]}", {binding["adapter"]}, {len(binding["arguments"])})'
        )
    declarations = banner + [
        "/** Audited utility surface. Inventory-only and pending mutation APIs are absent.",
        " * All methods enforce exact arity and strict primitive argument types.",
        " * Arguments are owned copies; native objects and pointers are never returned.",
        " */",
        "interface IrcdApi {",
    ]
    for binding in bindings:
        declarations.extend(
            [
                "  /**",
                f"   * {binding['description']}",
                f"   * Native: {binding['native']['symbol']}",
                f"   * Return conversion: {binding['returns']['conversion']}",
            ]
        )
        for argument in binding["arguments"]:
            declarations.append(f"   * @param {argument['name']} {argument['conversion']}")
        declarations.append("   */")
        args = ", ".join(f"{a['name']}: {a['type']}" for a in binding["arguments"])
        declarations.append(f"  {binding['js_name']}({args}): {binding['returns']['type']};")
    declarations.extend(["}", "", "declare const ircd: Readonly<IrcdApi>;"])
    expanded = dict(
        generated_by="tools/generate_js_bindings.py",
        manifest_sha256=digest,
        schema_version=data["schema_version"],
        api_namespace=data["api_namespace"],
        upstream=data["upstream"],
        policies=data["policies"],
        counts={
            "exposed": len(bindings),
            **{
                kind: sum(entry["classification"] == kind for entry in inventory)
                for kind in ("excluded", "requires_manual_wrapper", "planned_mutation")
            },
        },
        bindings=[dict(binding, exposed=True) for binding in bindings],
        inventory=[dict(entry, exposed=False) for entry in inventory],
    )
    return {
        OUTPUT_NAMES[0]: "\n".join(registration) + "\n",
        OUTPUT_NAMES[1]: "\n".join(declarations) + "\n",
        OUTPUT_NAMES[2]: json.dumps(expanded, indent=2, ensure_ascii=False, sort_keys=True) + "\n",
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--manifest", type=Path, default=ROOT / "bindings/inspircd.json")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "modules/js_generated")
    parser.add_argument(
        "--check", action="store_true", help="report missing/stale files without writing"
    )
    args = parser.parse_args(argv)
    try:
        outputs = render_outputs(load_manifest(args.manifest))
        if args.check:
            stale = []
            for name, expected in outputs.items():
                path = args.output_dir / name
                if not path.exists() or path.read_bytes() != expected.encode("utf-8"):
                    stale.append(str(path))
            if stale:
                print(
                    "Generated bindings are missing or stale:\n" + "\n".join(stale), file=sys.stderr
                )
                return 1
            print(f"PASS: {len(outputs)} generated binding files are current")
            return 0
        args.output_dir.mkdir(parents=True, exist_ok=True)
        for name, contents in outputs.items():
            (args.output_dir / name).write_bytes(contents.encode("utf-8"))
        print(f"Generated {len(outputs)} binding files in {args.output_dir}")
        return 0
    except (ManifestError, OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"Binding generation failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
