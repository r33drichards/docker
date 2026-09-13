#!/bin/sh
echo "
         ######################################
         ###        Maxchans config test     ##
         ###      (default and override)     ##
         ######################################
"


# Make sure tests fails if a command exits with non-zero
set -e

# shellcheck source=tests/.portconfig.sh
. "$(dirname "$0")/.portconfig.sh"

TESTFILE=$(mktemp /tmp/maxchansConfig.XXXXX)

MAXCHANS=42

mkdir -p "$(dirname "$TESTFILE")"

# Default: the define must be emitted as 256 when INSP_MAXCHANS is unset.
DOCKERCONTAINER=$(docker run -d -p "127.0.0.1:${CLIENT_PORT}:6667" -p "127.0.0.1:${TLS_CLIENT_PORT}:6697" inspircd:testing)

sleep 10

docker exec "${DOCKERCONTAINER}" /inspircd/conf/config.sh >"$TESTFILE"
grep "name=\"maxchans\" value=\"256\"" "$TESTFILE"

docker stop "${DOCKERCONTAINER}" && docker rm "${DOCKERCONTAINER}"

# Override: INSP_MAXCHANS must reach the define the connect class reads.
DOCKERCONTAINER=$(docker run -d -p "127.0.0.1:${CLIENT_PORT}:6667" -p "127.0.0.1:${TLS_CLIENT_PORT}:6697" -e "INSP_MAXCHANS=$MAXCHANS" inspircd:testing)

sleep 10

docker exec "${DOCKERCONTAINER}" /inspircd/conf/config.sh >"$TESTFILE"
grep "name=\"maxchans\" value=\"$MAXCHANS\"" "$TESTFILE"

# The connect class must consume the define rather than a hardcoded number.
docker exec "${DOCKERCONTAINER}" grep 'maxchans="&maxchans;"' /inspircd/conf/inspircd.conf

# Clean up
rm "$TESTFILE"
docker stop "${DOCKERCONTAINER}" && docker rm "${DOCKERCONTAINER}"
