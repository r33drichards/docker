#!/bin/sh
# Verify the normal image, without the CI-only hook probe.
set -eu
python3 "$(dirname "$0")/primary_image.py"
