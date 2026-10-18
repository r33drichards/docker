#!/usr/bin/env bash
# Required native memory-safety lanes. Never turn a missing dependency into SKIP.
set -euo pipefail

ROOT=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
MODE=${1:-all}
case "$MODE" in
    sanitizers|valgrind|lifecycle|all) ;;
    *) echo "Usage: $0 {sanitizers|valgrind|lifecycle|all}" >&2; exit 2 ;;
esac
if (( $# > 1 )); then
    echo "Usage: $0 {sanitizers|valgrind|lifecycle|all}" >&2
    exit 2
fi

CXX=${CXX:-g++}
VALGRIND=${VALGRIND:-valgrind}
LOG_DIR=${CI_NATIVE_LOG_DIR:-"$ROOT/test-results/native/$MODE"}
TEST_TIMEOUT=${CI_NATIVE_TIMEOUT:-180}
BUILD_TIMEOUT=${CI_NATIVE_BUILD_TIMEOUT:-180}
for seconds in "$TEST_TIMEOUT" "$BUILD_TIMEOUT"; do
    if [[ ! "$seconds" =~ ^[1-9][0-9]*$ ]]; then
        echo 'FAIL: timeouts must be positive integer seconds' >&2
        exit 2
    fi
done
for tool in "$CXX" timeout readelf; do
    command -v "$tool" >/dev/null || { echo "FAIL: required tool unavailable: $tool" >&2; exit 1; }
done
if [[ $(id -u) == 0 ]]; then
    echo 'FAIL: run native lanes as a non-root user so unreadable-file tests execute' >&2
    exit 1
fi
mkdir -p "$LOG_DIR"
LOG_DIR=$(CDPATH='' cd -- "$LOG_DIR" && pwd)
BUILD_DIR=$(mktemp -d)
cleanup() {
    chmod -R u+rwx "$BUILD_DIR"
    rm -rf -- "$BUILD_DIR"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

run_logged() {
    local name=$1
    shift
    local log="$LOG_DIR/$name.log"
    printf 'RUN: %s\n' "$name"
    printf 'Command: ' > "$log"
    printf '%q ' "$@" >> "$log"
    printf '\n' >> "$log"
    if "$@" >> "$log" 2>&1; then
        cat "$log"
    else
        local status=$?
        cat "$log" >&2
        echo "FAIL: $name exited $status (logs: $LOG_DIR)" >&2
        exit "$status"
    fi
}

COMMON=(-std=c++17 -g -Wall -Wextra -Werror -pedantic -fno-omit-frame-pointer -fno-optimize-sibling-calls)
V8_COMPILE=()
V8_LINK=()
VG_EXTRA=()
# These are whitespace-separated flag lists, not shell programs. No eval is used.
if [[ -n ${VALGRIND_EXTRA_FLAGS:-} ]]; then
    read -r -a VG_EXTRA <<< "$VALGRIND_EXTRA_FLAGS"
fi
setup_v8() {
    if [[ ! ${V8_CXXFLAGS+x} ]]; then
        if command -v pkg-config >/dev/null && pkg-config --exists libnode; then
            V8_CXXFLAGS=$(pkg-config --cflags libnode)
            V8_LDFLAGS=${V8_LDFLAGS:-$(pkg-config --libs libnode)}
        elif [[ -f /usr/include/node/v8.h ]]; then
            V8_CXXFLAGS='-isystem /usr/include/node'
        else
            echo 'FAIL: matched V8 headers/libraries required; set V8_CXXFLAGS and V8_LDFLAGS' >&2
            exit 1
        fi
    fi
    read -r -a V8_COMPILE <<< "$V8_CXXFLAGS"
    read -r -a V8_LINK <<< "${V8_LDFLAGS:--lnode}"
}
setup_valgrind() {
    command -v "$VALGRIND" >/dev/null || { echo "FAIL: required tool unavailable: $VALGRIND" >&2; exit 1; }
    run_logged valgrind-version "$VALGRIND" --version
}

run_logged compiler-version "$CXX" --version
{
    printf 'mode=%s\ncompiler=%s\ntimeout_seconds=%s\n' "$MODE" "$CXX" "$TEST_TIMEOUT"
    uname -a
    [[ ! -f /etc/os-release ]] || cat /etc/os-release
} > "$LOG_DIR/environment.log"

sanitizers() {
    setup_v8
    local detect_leaks=1
    if [[ ${CI_NATIVE_ALLOW_NO_LSAN:-0} == 1 ]]; then
        if [[ ${CI:-false} != false ]]; then
            echo 'FAIL: CI must keep LeakSanitizer enabled' >&2
            exit 1
        fi
        detect_leaks=0
        echo 'WARNING: local-only ASan/UBSan run; LeakSanitizer explicitly disabled' | tee "$LOG_DIR/lsan-disabled.log"
    fi
    local -a san=(-O1 "-fsanitize=address,undefined" -fno-sanitize-recover=all)
    local -a environment=(env "ASAN_OPTIONS=detect_leaks=$detect_leaks:halt_on_error=1:abort_on_error=1:detect_stack_use_after_return=1"
        'UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1' 'LSAN_OPTIONS=exitcode=23')
    for test in precommand_struct js_script_loader; do
        run_logged "build-$test-sanitizers" timeout --kill-after=10 "$BUILD_TIMEOUT" "$CXX" "${COMMON[@]}" "${san[@]}" \
            "$ROOT/tests/$test.cpp" -o "$BUILD_DIR/$test"
    done
    # Distro V8 is RTTI-free: vptr instrumentation would reference missing V8 typeinfo.
    # Other UBSan checks and all application ASan instrumentation remain enabled.
    run_logged build-js_runtime-sanitizers timeout --kill-after=10 "$BUILD_TIMEOUT" "$CXX" "${COMMON[@]}" "${san[@]}" \
        -fno-sanitize=vptr "${V8_COMPILE[@]}" "$ROOT/tests/js_runtime.cpp" "${V8_LINK[@]}" \
        -pthread -o "$BUILD_DIR/js_runtime"
    mkdir "$BUILD_DIR/sanitizer-fixtures"
    run_logged precommand_struct-sanitizers "${environment[@]}" timeout --kill-after=10 "$TEST_TIMEOUT" "$BUILD_DIR/precommand_struct"
    run_logged js_script_loader-sanitizers "${environment[@]}" timeout --kill-after=10 "$TEST_TIMEOUT" "$BUILD_DIR/js_script_loader" "$BUILD_DIR/sanitizer-fixtures"
    run_logged js_runtime-sanitizers "${environment[@]}" timeout --kill-after=10 "$TEST_TIMEOUT" "$BUILD_DIR/js_runtime"
}

memcheck() {
    local test=$1
    local leak_kinds=$2
    shift 2
    local log="$LOG_DIR/$test.memcheck.log"
    if timeout --kill-after=10 "$TEST_TIMEOUT" "$VALGRIND" "${VG_EXTRA[@]}" \
        --tool=memcheck --default-suppressions=no --error-exitcode=99 --leak-check=full \
        --show-leak-kinds=definite,indirect,possible --errors-for-leak-kinds="$leak_kinds" \
        --track-origins=yes --smc-check=all-non-file --log-file="$log" "$@" \
        > "$LOG_DIR/$test.stdout.log" 2>&1; then
        cat "$LOG_DIR/$test.stdout.log" "$log"
    else
        local status=$?
        cat "$LOG_DIR/$test.stdout.log" "$log" >&2
        echo "FAIL: $test exited $status (logs: $LOG_DIR)" >&2
        exit "$status"
    fi
}

valgrind_ownership() {
    setup_valgrind
    for test in precommand_struct js_script_loader; do
        run_logged "build-$test-valgrind" timeout --kill-after=10 "$BUILD_TIMEOUT" "$CXX" "${COMMON[@]}" -O0 \
            "$ROOT/tests/$test.cpp" -o "$BUILD_DIR/$test-valgrind"
    done
    mkdir "$BUILD_DIR/valgrind-fixtures"
    memcheck precommand_struct-valgrind all "$BUILD_DIR/precommand_struct-valgrind"
    memcheck js_script_loader-valgrind all "$BUILD_DIR/js_script_loader-valgrind" "$BUILD_DIR/valgrind-fixtures"
}

lifecycle() {
    setup_valgrind
    setup_v8
    local -a vg_headers=()
    if [[ -n ${VALGRIND_CXXFLAGS:-} ]]; then
        read -r -a vg_headers <<< "$VALGRIND_CXXFLAGS"
    fi
    # This lane intentionally requires GCC: -fno-gnu-unique excludes implicit
    # GNU-unique NODELETE so the explicit linker flag is actually under test.
    run_logged build-lifecycle-module timeout --kill-after=10 "$BUILD_TIMEOUT" "$CXX" "${COMMON[@]}" -O0 \
        -fPIC -shared -fvisibility=hidden -fno-gnu-unique -Wl,-z,nodelete "${V8_COMPILE[@]}" \
        "$ROOT/tests/js_runtime_lifecycle.cpp" "${V8_LINK[@]}" -pthread -o "$BUILD_DIR/lifecycle.so"
    run_logged build-lifecycle-host timeout --kill-after=10 "$BUILD_TIMEOUT" "$CXX" "${COMMON[@]}" -O0 \
        "${vg_headers[@]}" "$ROOT/tests/js_runtime_lifecycle_host.cpp" -ldl -o "$BUILD_DIR/lifecycle-host"
    readelf -d "$BUILD_DIR/lifecycle.so" > "$LOG_DIR/lifecycle-elf.log"
    grep -q NODELETE "$LOG_DIR/lifecycle-elf.log" || { echo 'FAIL: lifecycle DSO lacks NODELETE' >&2; exit 1; }
    # Process-global V8/platform allocations remain live by design. The host
    # separately requires zero growth in possible AND reachable bytes after
    # warmup; initial totals are printed, not hidden by a suppression file.
    memcheck js_runtime-lifecycle definite,indirect "$BUILD_DIR/lifecycle-host" "$BUILD_DIR/lifecycle.so"
}

case "$MODE" in
    sanitizers) sanitizers ;;
    valgrind) valgrind_ownership ;;
    lifecycle) lifecycle ;;
    all) sanitizers; valgrind_ownership; lifecycle ;;
esac
printf 'PASS: native %s lane (logs: %s)\n' "$MODE" "$LOG_DIR"
