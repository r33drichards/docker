#!/bin/sh
# Standalone pure-C++ SQLite backend tests.
# Override SQLITE_CXXFLAGS/SQLITE_LDFLAGS for a locally extracted SDK.
# CXXFLAGS/LDFLAGS can add sanitizers or other compiler/linker options.
set -eu
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
TESTDIR=$(mktemp -d)
trap 'rm -rf "$TESTDIR"' EXIT
trap 'exit 1' HUP INT TERM

if [ "${SQLITE_CXXFLAGS+x}" != x ]; then
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists sqlite3; then
        SQLITE_CXXFLAGS=$(pkg-config --cflags sqlite3)
        SQLITE_LDFLAGS=${SQLITE_LDFLAGS-$(pkg-config --libs sqlite3)}
    elif [ -f /usr/include/sqlite3.h ]; then
        SQLITE_CXXFLAGS=''
    else
        echo 'ERROR: SQLite development files required; set SQLITE_CXXFLAGS and SQLITE_LDFLAGS' >&2
        exit 1
    fi
fi
SQLITE_LDFLAGS=${SQLITE_LDFLAGS-'-lsqlite3'}
# Word splitting is intentional for caller-supplied compiler/linker flag lists.
# shellcheck disable=SC2086
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic \
    ${CXXFLAGS-} $SQLITE_CXXFLAGS "$SCRIPT_DIR/test_js_sqlite.cpp" \
    ${LDFLAGS-} $SQLITE_LDFLAGS -o "$TESTDIR/test_js_sqlite"
if command -v timeout >/dev/null 2>&1; then
    timeout 30 "$TESTDIR/test_js_sqlite"
else
    "$TESTDIR/test_js_sqlite"
fi
