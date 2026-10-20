#!/bin/sh
set -eu
if [ -z "${INSPIRCD_NATIVE:-}" ]; then
    echo 'SKIP: set INSPIRCD_NATIVE to a built native installation with m_javascript.so'
    exit 0
fi
python3 "$(dirname "$0")/javascript_native.py"

python3 "$(dirname "$0")/native_host_api.py"

if [ "${INSPIRCD_HTTP_TEST_LOOPBACK:-}" = "1" ]; then
    python3 "$(dirname "$0")/fetch_headers_native.py"
fi
