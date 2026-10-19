#!/bin/sh
# SPDX-License-Identifier: MIT
# Bounded real libFuzzer + ASan/UBSan runs. Missing dependencies are failures.
set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
CXX=${CXX:-clang++-19}
FUZZ_SECONDS=${FUZZ_SECONDS:-10}
FUZZ_SEED=${FUZZ_SEED:-20261018}
FUZZ_DETECT_LEAKS=${FUZZ_DETECT_LEAKS:-1}
for dependency in "$CXX" python3 timeout; do
    if ! command -v "$dependency" >/dev/null 2>&1; then
        echo "Required fuzz dependency missing: $dependency" >&2
        echo "On Debian trixie: apt-get install clang-19 libclang-rt-19-dev python3 coreutils" >&2
        exit 1
    fi
done
case "$FUZZ_SECONDS" in
    ''|*[!0-9]*|0*) echo "FUZZ_SECONDS must be a positive integer without leading zeros" >&2; exit 1 ;;
esac
case "$FUZZ_SEED" in
    ''|*[!0-9]*|0*) echo "FUZZ_SEED must be a positive integer without leading zeros" >&2; exit 1 ;;
esac
if [ -n "${CI:-}" ] && [ "$FUZZ_DETECT_LEAKS" != 1 ]; then
    echo "Leak detection cannot be disabled in CI" >&2
    exit 1
fi
case "$FUZZ_DETECT_LEAKS" in
    0|1) ;;
    *) echo "FUZZ_DETECT_LEAKS must be 0 or 1" >&2; exit 1 ;;
esac
if [ "${#FUZZ_SECONDS}" -gt 4 ] || [ "$FUZZ_SECONDS" -gt 3600 ]; then
    echo "FUZZ_SECONDS must not exceed 3600 seconds per target" >&2
    exit 1
fi
if [ "${#FUZZ_SEED}" -gt 10 ] || [ "$FUZZ_SEED" -gt 4294967295 ]; then
    echo "FUZZ_SEED must not exceed 4294967295" >&2
    exit 1
fi
if ! "$CXX" --version | grep -qi clang; then
    echo "CXX must select Clang with its matching libFuzzer runtime" >&2
    exit 1
fi

FUZZ_OUT=${FUZZ_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/irc-js-fuzz-XXXXXX")}
mkdir -p "$FUZZ_OUT/bin" "$FUZZ_OUT/logs" "$FUZZ_OUT/artifacts" "$FUZZ_OUT/corpus"
FUZZ_OUT=$(CDPATH='' cd -- "$FUZZ_OUT" && pwd)
printf 'Fuzz output (retained): %s\n' "$FUZZ_OUT"
"$CXX" --version > "$FUZZ_OUT/compiler.txt"
printf 'FUZZ_SECONDS=%s\nFUZZ_SEED=%s\nFUZZ_DETECT_LEAKS=%s\n' \
    "$FUZZ_SECONDS" "$FUZZ_SEED" "$FUZZ_DETECT_LEAKS" > "$FUZZ_OUT/settings.txt"
python3 "$SCRIPT_DIR/generate_seeds.py" "$FUZZ_OUT/corpus"

# Do not permit UBSan to log undefined behavior and then return success.
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}abort_on_error=1:detect_leaks=$FUZZ_DETECT_LEAKS"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:+$UBSAN_OPTIONS:}halt_on_error=1:print_stacktrace=1"
if [ "$FUZZ_DETECT_LEAKS" = 0 ]; then
    echo "WARNING: LeakSanitizer explicitly disabled; AddressSanitizer and UBSan remain enabled" >&2
fi
for target in wire_validation parsed_command script_loader; do
    binary="$FUZZ_OUT/bin/$target"
    artifacts="$FUZZ_OUT/artifacts/$target"
    mkdir -p "$artifacts"
    echo "Building $target with libFuzzer, AddressSanitizer and UndefinedBehaviorSanitizer"
    timeout --kill-after=10s 120s "$CXX" \
        -std=c++17 -O1 -g -Wall -Wextra -Werror -pedantic \
        -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined \
        -fno-sanitize-recover=all "$SCRIPT_DIR/$target.cpp" -o "$binary"
    echo "Fuzzing $target for $FUZZ_SECONDS seconds (seed $FUZZ_SEED)"
    # The shell watchdog bounds engine startup/shutdown too; any timeout fails.
    # libFuzzer saves exact crash/leak/timeout inputs under artifacts/target.
    if timeout --kill-after=10s "$((FUZZ_SECONDS + 30))s" "$binary" \
        "$FUZZ_OUT/corpus/$target" \
        "-max_total_time=$FUZZ_SECONDS" "-seed=$FUZZ_SEED" \
        -max_len=4096 -timeout=5 -rss_limit_mb=1024 -malloc_limit_mb=128 \
        -print_final_stats=1 "-artifact_prefix=$artifacts/" \
        > "$FUZZ_OUT/logs/$target.log" 2>&1; then
        tail -n 12 "$FUZZ_OUT/logs/$target.log"
    else
        status=$?
        cat "$FUZZ_OUT/logs/$target.log" >&2
        printf 'FAILED: %s (exit %s). Reproduce: %s -runs=1 %s/<artifact>\n' \
            "$target" "$status" "$binary" "$artifacts" >&2
        exit "$status"
    fi
done
echo "All three bounded fuzz targets passed (leak detection=$FUZZ_DETECT_LEAKS); outputs retained in $FUZZ_OUT"
