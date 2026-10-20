#!/bin/sh
# Explicitly enable the fixed persistent SQLite directory; never grant scripts implicitly.
case ${INSPIRCD_SQLITE:-no} in
    no) exit 0 ;;
    yes) ;;
    *) printf '%s\n' '<die reason="INSPIRCD_SQLITE must be yes or no">'; exit 1 ;;
esac
# The existing NocoDB include already loads this module when enabled.
if [ "${INSPIRCD_NOCODB_WCHAT:-no}" != yes ]; then
    printf '%s\n' '<module name="javascript">'
fi
printf '%s\n' '<javascriptsettings sqlitedirectory="/inspircd/data/script-databases">'
