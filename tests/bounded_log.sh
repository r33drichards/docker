#!/bin/sh
# Standalone bounded logging backend tests; no daemon or privileges required.
set -eu
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
TESTDIR=$(mktemp -d)
trap 'rm -rf "$TESTDIR"' EXIT
trap 'exit 1' HUP INT TERM
# Word splitting permits caller-supplied sanitizer/compiler/linker flags.
# shellcheck disable=SC2086
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic \
    ${CXXFLAGS-} "$SCRIPT_DIR/test_bounded_log.cpp" ${LDFLAGS-} -o "$TESTDIR/test_bounded_log"
"$TESTDIR/test_bounded_log"
