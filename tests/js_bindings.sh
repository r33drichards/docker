#!/bin/sh
# Validate the manifest, deterministic output, and absence of unapproved APIs.
# This test is independent of V8, Node, Docker, and a running IRC server.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export PYTHONDONTWRITEBYTECODE=1
"${PYTHON:-python3}" "$ROOT/tools/generate_js_bindings.py" --check
"${PYTHON:-python3}" "$ROOT/tests/test_generate_js_bindings.py"
