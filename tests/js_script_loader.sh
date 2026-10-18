#!/bin/sh
# Test JS source discovery and owned bounded reads without InspIRCd or V8.
set -eu

TESTDIR=$(mktemp -d)
trap 'chmod -R u+rwx "$TESTDIR"; rm -rf "$TESTDIR"' EXIT
trap 'exit 1' HUP INT TERM
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic \
    "$SCRIPT_DIR/js_script_loader.cpp" -o "$TESTDIR/js_script_loader"
"$TESTDIR/js_script_loader" "$TESTDIR"
