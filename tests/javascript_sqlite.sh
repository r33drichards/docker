#!/bin/sh
set -eu
if [ -z "${INSPIRCD_NATIVE:-}" ]; then
    echo 'SKIP: set INSPIRCD_NATIVE to a built native installation with SQLite-enabled m_javascript.so'
    exit 0
fi
PYTHONDONTWRITEBYTECODE=1 python3 "$(dirname "$0")/javascript_sqlite_native.py"
