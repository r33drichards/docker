#!/bin/sh
# Test the exact owned struct used by OnPreCommand, without an IRC server.
set -eu

TESTDIR=$(mktemp -d)
trap 'rm -rf "$TESTDIR"' EXIT
trap 'exit 1' HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic \
    tests/precommand_struct.cpp -o "$TESTDIR/precommand_struct"
"$TESTDIR/precommand_struct"
echo "PASS: owned unredacted values and deterministic byte-preserving repr"
