#!/bin/sh
echo "
         ######################################
         ###      httpd stats test          ###
         ######################################
"


# Make sure tests fails if a command exits with non-zero
set -e

# shellcheck source=tests/.portconfig.sh
. "$(dirname "$0")/.portconfig.sh"

HTTPD_PORT=$(cat /dev/urandom|od -N2 -An -i|awk -v f=50000 -v r=9999 '{printf "%i\n", f + r * $1 / 65536}')
STATS_PASSWORD="testpassword"

# The listener is opt-in, so a default container must not open it.
DEFAULTCONTAINER=$(docker run -d -p "127.0.0.1:${CLIENT_PORT}:6667" inspircd:testing)
sleep 10
docker exec "${DEFAULTCONTAINER}" sh -c '! netstat -ln 2>/dev/null | grep -q ":8067 "' \
    || { echo >&2 "httpd listener opened without INSP_HTTPD_ENABLE, test failed!"; exit 1; }
docker stop "${DEFAULTCONTAINER}" && docker rm "${DEFAULTCONTAINER}"

# With the listener enabled /stats/general must serve the counters, and must
# require the configured password.
DOCKERCONTAINER=$(docker run -d \
    -e "INSP_HTTPD_ENABLE=yes" \
    -e "INSP_HTTPD_PORT=8067" \
    -e "INSP_HTTPD_PASSWORD=${STATS_PASSWORD}" \
    -p "127.0.0.1:${HTTPD_PORT}:8067" \
    inspircd:testing)
sleep 10

# Without credentials the ACL must reject the request.
UNAUTHENTICATED=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:${HTTPD_PORT}/stats/general")
[ "${UNAUTHENTICATED}" != "200" ] \
    || { echo >&2 "/stats/general served without a password, test failed!"; exit 1; }

# With credentials it must return the general block.
curl -s -u "stats:${STATS_PASSWORD}" "http://127.0.0.1:${HTTPD_PORT}/stats/general" | grep -q "usercount" \
    || { echo >&2 "/stats/general did not return usercount, test failed!"; exit 1; }

# Clean up
docker stop "${DOCKERCONTAINER}" && docker rm "${DOCKERCONTAINER}"
