#!/bin/sh
# Verified official release digest; never execute an unchecked download.
set -eu
VERSION=1.7.12
SHA256=8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
trap 'exit 1' HUP INT TERM
curl --fail --location --retry 3 --proto '=https' --tlsv1.2 \
    "https://github.com/rhysd/actionlint/releases/download/v${VERSION}/actionlint_${VERSION}_linux_amd64.tar.gz" \
    -o "$DIR/actionlint.tar.gz"
printf '%s  %s\n' "$SHA256" "$DIR/actionlint.tar.gz" | sha256sum -c -
tar -xzf "$DIR/actionlint.tar.gz" -C "$DIR" actionlint
cd "$ROOT"
"$DIR/actionlint" -version
"$DIR/actionlint" -color .github/workflows/*.yml
