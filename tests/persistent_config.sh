#!/bin/sh
# Isolated fixed-path bootstrap and checksum-managed configuration regressions.
set -eu
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
python3 "$SCRIPT_DIR/test_sqlite_bootstrap.py"
python3 "$SCRIPT_DIR/test_persistent_config.py"
