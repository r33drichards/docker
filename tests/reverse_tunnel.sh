#!/bin/sh
set -eu
TESTDIR=$(mktemp -d)
trap 'rm -rf "$TESTDIR"' EXIT
trap 'exit 1' HUP INT TERM
for source in tests/test_reverse_tunnel.cpp tests/test_reverse_tunnel_controller.cpp; do
    # shellcheck disable=SC2086
    "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic ${CXXFLAGS-} \
        "$source" ${LDFLAGS-} -o "$TESTDIR/test"
    "$TESTDIR/test"
done
docker build -t inspircd-tunnel:integration tests/reverse-tunnel
docker run --rm --network none inspircd-tunnel:integration
docker build -f tests/reverse-tunnel/Dockerfile.real -t inspircd-tunnel:real-forwarding tests/reverse-tunnel
docker run --rm --network none inspircd-tunnel:real-forwarding
