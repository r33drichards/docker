#!/bin/sh
# Check default and opt-in hook behavior, using synthetic data only.
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

# cap, account, and gateway are already loaded by the image's default config.
# Register AUTHENTICATE too, to check full values even when its handler fails.
printf '<module name="sasl">\n<sasl target="services.test">\n' > "$TESTDIR/precommand.conf"
for mode in unloaded loaded; do
    if [ "$mode" = loaded ]; then
        cat >> "$TESTDIR/precommand.conf" <<'CONF'
<module name="precommand">
<log method="stdout" level="normal" type="m_precommand">
CONF
    fi
    chmod 644 "$TESTDIR/precommand.conf"
    DOCKERCONTAINER=$(docker run -d -e INSP_ENABLE_DNSBL=no \
        -v "$TESTDIR/precommand.conf:/inspircd/conf.d/precommand.conf:ro" \
        -p 127.0.0.1::6667 inspircd:testing)
    PORT=$(docker port "$DOCKERCONTAINER" 6667/tcp | sed 's/.*://')
    python3 tests/precommand_client.py 127.0.0.1 "$PORT"
    # Stop cleanly so buffered stdout is flushed before reading it.
    docker stop "$DOCKERCONTAINER" >/dev/null
    docker logs "$DOCKERCONTAINER" > "$TESTDIR/output.log" 2>&1
    if [ "$mode" = unloaded ]; then
        python3 tests/precommand_log.py "$TESTDIR/output.log" --expect-unloaded
    else
        python3 tests/precommand_log.py "$TESTDIR/output.log"
    fi
    docker rm "$DOCKERCONTAINER" >/dev/null
    DOCKERCONTAINER=
done
