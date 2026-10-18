#!/bin/sh
set -eu
if [ -z "${INSPIRCD_NATIVE:-}" ]; then
    echo 'SKIP: set INSPIRCD_NATIVE to a built native installation with m_javascript.so'
    exit 0
fi
python3 "$(dirname "$0")/javascript_native.py"
