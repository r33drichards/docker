# Test module changes

Run from the repository root. Never point integration tests at an existing server
configuration. Build the pinned InspIRCd SDK and V8 dependencies before native tests.

~~~sh
python3 tools/generate_module_hooks.py --check
python3 tests/test_generate_module_hooks.py
sh tests/js_bindings.sh
sh tests/js_script_loader.sh
sh tests/js_runtime.sh
~~~

The runtime script reports SKIP when matching V8 dependencies are missing; a skip
is not a pass. You may supply `V8_CXXFLAGS` and `V8_LDFLAGS` for matching libraries.
For ABI parity against an inspected SDK:

~~~sh
python3 tools/generate_module_hooks.py --check --upstream /path/to/inspircd/include/modules.h
~~~

The full native suite needs an isolated installation with both `m_javascript.so`
and the test-only `m_js_hook_probe.so`. The optional Docker build supports
`--build-arg TEST_HOOK_PROBE=1` for this purpose; never ship that probe in a normal image.

~~~sh
INSPIRCD_NATIVE=/path/to/installation sh tests/javascript.sh
~~~

The probe directly invokes all 75 adapters, checks owned snapshots and decision
matrices, then exercises selected real lifecycle/message events. This is not
natural-trigger E2E coverage for every hook. Consult the repository's
[CI coverage](https://github.com/r33drichards/docker/blob/feat/local-echo-command/docs/CI.md)
for quick checks, sanitizers, Valgrind, fuzzing and label-triggered heavy image runs.
