#!/bin/sh
# shellcheck disable=SC3028,SC2039

########################################
###                                  ###
### DON'T EDIT THIS FILE AFTER BUILD ###
###                                  ###
###    USE ENVIRONMENT VARIABLES     ###
###              INSTEAD             ###
###                                  ###
########################################

# Emits the httpd/httpd_stats configuration used to expose server statistics
# over HTTP, so that an external collector can turn them into metrics.
#
# Disabled unless INSP_HTTPD_ENABLE is "yes", because httpd_stats exposes
# sensitive information (connected users, their hosts and IP addresses) and
# must never be reachable from an untrusted network.
#
#   INSP_HTTPD_ENABLE    yes to load the modules and open the listener (default: no)
#   INSP_HTTPD_ADDRESS   listener address; empty means all interfaces and, on
#                        dual-stack hosts, both IPv4 and IPv6 (default: empty).
#                        An explicit "::" makes InspIRCd set IPV6_V6ONLY, so
#                        leave this empty unless you want one family only.
#   INSP_HTTPD_PORT      listener port (default: 8067)
#   INSP_HTTPD_TIMEOUT   seconds before an HTTP connection is closed (default: 20)
#   INSP_HTTPD_USERNAME  HTTP basic auth username for /stats (default: stats)
#   INSP_HTTPD_PASSWORD  HTTP basic auth password for /stats; when set, the
#                        httpd_acl module is loaded and /stats requires it.

[ "${INSP_HTTPD_ENABLE:-no}" = "yes" ] || exit 0

cat <<EOF
<module name="httpd">
<module name="httpd_stats">
<httpd timeout="${INSP_HTTPD_TIMEOUT:-20}">
<bind address="${INSP_HTTPD_ADDRESS:-}" port="${INSP_HTTPD_PORT:-8067}" type="httpd">
EOF

if [ -n "${INSP_HTTPD_PASSWORD}" ]; then
    cat <<EOF
<module name="httpd_acl">
<httpdacl path="/stats*" types="password" username="${INSP_HTTPD_USERNAME:-stats}" password="${INSP_HTTPD_PASSWORD}">
EOF
fi
