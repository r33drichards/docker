#!/usr/bin/env python3
"""Full pinned Module ABI inventory, adapters and schema drift regressions."""

import copy
import importlib.util
import json
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location(
    "module_hooks", ROOT / "tools/generate_module_hooks.py"
)
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class HookGenerationTests(unittest.TestCase):
    def setUp(self):
        self.data = json.loads(generator.MANIFEST.read_text())
        self.hooks = generator.validate(self.data)

    def test_all_native_hooks_are_present(self):
        self.assertEqual(75, len(self.hooks))
        self.assertEqual(74, len([h for h in self.hooks if h["native"] != "OnCleanup"]))
        self.assertEqual(2, len([h for h in self.hooks if h["adapter"] == "command"]))

    def test_all_outputs_are_current(self):
        for path, expected in generator.outputs(self.hooks).items():
            with self.subTest(path=path):
                self.assertEqual(expected, (ROOT / path).read_text())

    def test_every_native_hook_has_a_real_call(self):
        calls = generator.outputs(self.hooks)["tests/fixtures/module_hook_calls.inc"]
        for hook in self.hooks:
            self.assertEqual(1, calls.count("target->" + hook["native"] + "("))

    def test_every_hook_has_registration_and_types(self):
        outputs = generator.outputs(self.hooks)
        for hook in self.hooks:
            self.assertIn('"' + hook["js"] + '"', outputs["modules/js_generated/hook_specs.h"])
            if hook["adapter"] == "generated":
                self.assertIn(
                    hook["native"] + "(", outputs["modules/js_generated/hook_adapters.inc"]
                )
                self.assertIn(hook["js"] + ":", outputs["modules/js_generated/module_hooks.d.ts"])
                for arg in hook["args"]:
                    self.assertNotIn("any", arg["ts"])
                    self.assertNotIn("*", arg["ts"])

    def test_duplicates_are_rejected(self):
        self.data["hooks"].append(copy.deepcopy(self.hooks[0]))
        with self.assertRaisesRegex(ValueError, "duplicate"):
            generator.validate(self.data)

    def test_signature_drift_is_rejected(self):
        self.hooks[0]["signature"] = "User* user"
        with self.assertRaisesRegex(ValueError, "signature"):
            generator.validate(self.data)

    def test_unknown_adapter_is_rejected(self):
        self.hooks[0]["adapter"] = "skip"
        with self.assertRaisesRegex(ValueError, "adapter"):
            generator.validate(self.data)

    def test_only_reviewed_mutation_adapters_are_generated(self):
        self.assertEqual(5, len(generator.MUTATIONS))
        outputs = generator.outputs(self.hooks)
        self.assertEqual(
            5, outputs["modules/js_generated/hook_adapters.inc"].count("mutation.Apply(")
        )
        for hook in self.hooks:
            self.assertEqual(hook["js"] in generator.MUTATIONS, "mutation" in hook)

    def test_mutation_schema_drift_is_rejected(self):
        selected = next(h for h in self.hooks if h["js"] == "userPreMessage")
        selected.pop("mutation")
        with self.assertRaisesRegex(ValueError, "mutation"):
            generator.validate(self.data)
        selected["mutation"] = "userPreMessage"
        self.hooks[0]["mutation"] = "userPreMessage"
        with self.assertRaisesRegex(ValueError, "mutation"):
            generator.validate(self.data)

    def test_socket_ownership_is_not_exposed(self):
        hook = next(h for h in self.hooks if h["native"] == "OnAcceptConnection")
        self.assertFalse(hook["allow"])
        self.assertEqual("deny", hook["failure"])

    def test_cleanup_does_not_fail_closed(self):
        for name in ("OnKill", "OnChannelPreDelete", "OnUserPreQuit"):
            hook = next(h for h in self.hooks if h["native"] == name)
            self.assertEqual("pass", hook["failure"])

    def test_upstream_add_remove_change_and_table_drift(self):
        header = (
            "enum Implementation {"
            + ",".join("I_" + h["native"] for h in self.hooks if h["native"] != "OnCleanup")
            + "};\nclass CoreExport Module {\n"
        )
        header += (
            "\n".join(f"virtual {h['result']} {h['native']}({h['signature']});" for h in self.hooks)
            + "};"
        )

        class Source:
            def read_text(self):
                return header

        generator.upstream_check(self.hooks, Source())
        for changed in (
            header.replace("virtual void OnUserConnect(LocalUser* user);", ""),
            header + "virtual void OnNewHook();",
            header.replace("OnUserConnect(LocalUser* user)", "OnUserConnect(User* user)"),
            header.replace("I_OnUserConnect,", ""),
        ):
            original = header
            header = changed
            with self.assertRaisesRegex(ValueError, "drift|table"):
                generator.upstream_check(self.hooks, Source())
            header = original


if __name__ == "__main__":
    unittest.main()
