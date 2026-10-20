#!/bin/sh
# Compile the real module against a separately checked-out, pinned upstream tree.
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
: "${INSPIRCD_SOURCE:?Set INSPIRCD_SOURCE to the upstream checkout}"
: "${INSPIRCD_SHA:?Set INSPIRCD_SHA to its expected full commit SHA}"
test "$(git -C "$INSPIRCD_SOURCE" rev-parse HEAD)" = "$INSPIRCD_SHA"
cp "$ROOT"/modules/js_*.h "$ROOT"/modules/js_*.inc "$INSPIRCD_SOURCE/src/modules/"
mkdir -p "$INSPIRCD_SOURCE/src/modules/js_generated"
cp "$ROOT"/modules/js_generated/* "$INSPIRCD_SOURCE/src/modules/js_generated/"
cp "$ROOT/modules/extra/m_javascript.cpp" "$INSPIRCD_SOURCE/src/modules/extra/"
python3 "$ROOT/tools/generate_module_hooks.py" --check --upstream "$INSPIRCD_SOURCE/include/modules.h"
cp "$ROOT/tests/fixtures/m_js_event_probe.cpp" "$INSPIRCD_SOURCE/src/modules/"
cp "$ROOT/tests/fixtures/m_js_hook_probe.cpp" "$INSPIRCD_SOURCE/src/modules/"
cp "$ROOT/tests/fixtures/module_hook_calls.inc" "$INSPIRCD_SOURCE/src/modules/"
cd "$INSPIRCD_SOURCE"
./configure --enable-extras=javascript
./configure --disable-auto-extras --disable-ownership --prefix=/tmp/inspircd-ci
INSPIRCD_TARGET=m_javascript make
INSPIRCD_TARGET=m_js_hook_probe make
INSPIRCD_TARGET=m_js_event_probe make
MODULE=$(find build -path '*/modules/m_javascript.so' -type f -print -quit)
test -n "$MODULE"
readelf -d "$MODULE" | grep NODELETE
readelf -d "$MODULE" | grep libnode
