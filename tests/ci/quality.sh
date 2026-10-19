#!/bin/sh
# Required, read-only checks for the new native module and its test tooling.
# Keep this below tests/ci/: run_tests.sh discovers ordinary tests/*.sh.
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT"
export LC_ALL=C
export PYTHONDONTWRITEBYTECODE=1

CLANG_FORMAT=${CLANG_FORMAT:-clang-format-19}
RUFF=${RUFF:-ruff}
SHELLCHECK=${SHELLCHECK:-shellcheck}
CPPCHECK=${CPPCHECK:-cppcheck}
PYTHON=${PYTHON:-python3}

fail()
{
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

for tool in "$CLANG_FORMAT" "$RUFF" "$SHELLCHECK" "$CPPCHECK" "$PYTHON"; do
    command -v "$tool" >/dev/null 2>&1 || fail "required tool not found: $tool (see docs/CI_QUALITY.md)"
done

# Formatter output and lint rules must not silently change with the host PATH.
case $("$CLANG_FORMAT" --version) in
    *'clang-format version 19.'*) ;;
    *) fail 'clang-format major version 19 is required' ;;
esac
[ "$("$RUFF" --version)" = 'ruff 0.16.9' ] || fail 'Ruff 0.16.9 is required'
[ "$("$SHELLCHECK" --version | sed -n 's/^version: //p')" = '0.10.0' ] || fail 'ShellCheck 0.10.0 is required'
[ "$("$CPPCHECK" --version)" = 'Cppcheck 2.17.1' ] || fail 'Cppcheck 2.17.1 is required'
"$PYTHON" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else "Python 3.10+ is required")'

check_cpp()
{
    "$CLANG_FORMAT" --dry-run --Werror "$@"
    # Parse owned headers as C++, even when passed directly. System headers and
    # vendored daemon/V8 internals are deliberately outside this lint scope;
    # the native compile jobs validate those complete integrations.
    "$CPPCHECK" --language=c++ --std=c++17 --enable=warning,performance,portability \
        --check-level=exhaustive --inline-suppr --error-exitcode=1 "$@"
}

check_cpp \
    modules/m_precommand.cpp modules/parsed_command.h \
    modules/extra/m_javascript.cpp modules/js_native_utilities.h \
    modules/js_webhooks.h tests/js_webhooks.cpp \
    modules/js_runtime.h modules/js_script_loader.h modules/js_wire_validation.h modules/js_hook_snapshot.h \
    tests/fixtures/m_js_hook_probe.cpp \
    tests/precommand_struct.cpp tests/js_runtime.cpp tests/js_script_loader.cpp \
    tests/js_runtime_lifecycle.cpp tests/js_runtime_lifecycle_host.cpp \
    tests/fuzz/*.cpp tests/fuzz/*.h

check_python()
{
    "$RUFF" check --no-cache "$@"
    "$RUFF" format --no-cache --check "$@"
}

check_python \
    tools/generate_js_bindings.py tools/generate_module_hooks.py \
    tests/test_generate_module_hooks.py tests/test_generate_js_bindings.py tests/test_authorize_heavy.py \
    tests/javascript_native.py tests/precommand_client.py tests/test_precommand_client.py \
    tests/precommand_log.py tests/primary_image.py tests/test_js_webhooks.py \
    tests/ci/*.py tests/fuzz/*.py

"$SHELLCHECK" --severity=style \
    tests/primaryImage.sh tests/javascript.sh tests/js_bindings.sh tests/js_runtime.sh \
    tests/js_script_loader.sh tests/precommand.sh tests/precommand_struct.sh \
    tests/ci/*.sh tests/fuzz/*.sh

# Generated files have one canonical formatter: the generator itself.
"$PYTHON" tools/generate_js_bindings.py --check
"$PYTHON" tests/test_generate_js_bindings.py
"$PYTHON" tools/generate_module_hooks.py --check
"$PYTHON" tests/test_generate_module_hooks.py
"$PYTHON" tests/test_authorize_heavy.py
"$PYTHON" tests/test_precommand_client.py
printf '%s\n' 'PASS: C++/Python formatting, static analysis, shell lint, generator drift, CI selector, and registration tests'
