#!/bin/sh
# Verify both the default (unloaded) image and the opt-in ECHO module.
set -eu

TESTDIR=$(mktemp -d)
DOCKERCONTAINER=
cleanup() {
    status=$?
    trap - EXIT
    if [ -n "$DOCKERCONTAINER" ]; then
        if [ "$status" -ne 0 ]; then
            docker logs "$DOCKERCONTAINER" || true
        fi
        docker rm -f "$DOCKERCONTAINER" >/dev/null || true
    fi
    rm -rf "$TESTDIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

DOCKERCONTAINER=$(docker run -d -e INSP_ENABLE_DNSBL=no \
    -p 127.0.0.1::6667 inspircd:testing)
PORT=$(docker port "$DOCKERCONTAINER" 6667/tcp | sed 's/.*://')
python3 tests/echo_client.py 127.0.0.1 "$PORT" --expect-unloaded
docker rm -f "$DOCKERCONTAINER" >/dev/null
DOCKERCONTAINER=

printf '<module name="echo">\n' > "$TESTDIR/echo.conf"
chmod 644 "$TESTDIR/echo.conf"
DOCKERCONTAINER=$(docker run -d -e INSP_ENABLE_DNSBL=no \
    -v "$TESTDIR/echo.conf:/inspircd/conf.d/echo.conf:ro" \
    -p 127.0.0.1::6667 inspircd:testing)
PORT=$(docker port "$DOCKERCONTAINER" 6667/tcp | sed 's/.*://')
python3 tests/echo_client.py 127.0.0.1 "$PORT"
