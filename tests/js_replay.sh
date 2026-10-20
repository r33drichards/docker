#!/bin/sh
# Standalone embedding tests; use matched V8 headers and shared libraries.
# Example: V8_CXXFLAGS='-isystem /path/include/node' \
#          V8_LDFLAGS='-L/path/lib -Wl,-rpath,/path/lib -lnode' ./tests/js_runtime.sh
set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
TESTDIR=$(mktemp -d)
trap 'rm -rf "$TESTDIR"' EXIT
trap 'exit 1' HUP INT TERM

if [ "${V8_CXXFLAGS+x}" != x ]; then
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libnode; then
        V8_CXXFLAGS=$(pkg-config --cflags libnode)
        V8_LDFLAGS=${V8_LDFLAGS-$(pkg-config --libs libnode)}
    elif [ -f /usr/include/node/v8.h ]; then
        V8_CXXFLAGS='-isystem /usr/include/node'
    else
        echo 'SKIP: matched V8 development files unavailable; set V8_CXXFLAGS and V8_LDFLAGS'
        exit 0
    fi
fi
V8_LDFLAGS=${V8_LDFLAGS-'-lnode'}
# Word splitting is intentional for caller-supplied compiler/linker flag lists.
# shellcheck disable=SC2086
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic \
    $V8_CXXFLAGS "$SCRIPT_DIR/js_replay.cpp" $V8_LDFLAGS \
    -pthread -o "$TESTDIR/js_replay"
if [ "$#" -eq 0 ]; then
    set -- "$SCRIPT_DIR"/replay/scenarios/*.js
fi
# Bound the complete test as a second line of defense against a broken watchdog.
if command -v timeout >/dev/null 2>&1; then
    timeout 30 "$TESTDIR/js_replay" "$SCRIPT_DIR/replay/host.js" "$@"
else
    "$TESTDIR/js_replay" "$SCRIPT_DIR/replay/host.js" "$@"
fi
