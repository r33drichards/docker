#!/bin/sh
# Fixed, explicitly opt-in #wchat forwarding. Never read or print token contents.
fail() {
    # Executable include exit status alone is not a reliable config rejection.
    printf '%s\n' '<die reason="NocoDB forwarding requires valid activation settings and a protected token file">'
    exit 1
}
case ${INSPIRCD_NOCODB_WCHAT:-no} in
    no) exit 0 ;;
    yes) ;;
    *) fail ;;
esac
secret=/inspircd/.secrets/nocodb-token
if ! { [ ! -L "$secret" ] && [ -f "$secret" ] && [ -r "$secret" ]; }; then fail; fi
[ "$(stat -c %u "$secret")" = "$(id -u)" ] || fail
case $(stat -c %a "$secret") in 400|600) ;; *) fail ;; esac
size=$(stat -c %s "$secret") || fail
if ! { [ "$size" -ge 1 ] && [ "$size" -le 1024 ]; }; then fail; fi
cat <<'CONFIG'
<module name="javascript">
<javascriptwebhook name="wchat_events" url="https://nocodb-render-mvp.onrender.com/api/v2/tables/mdz86ygcx26cnd6/records">
<javascriptwebhookheader endpoint="wchat_events" name="xc-token" valuefile="/inspircd/.secrets/nocodb-token">
<javascript file="/inspircd/scripts/wchat-nocodb.js" authority="admin">
CONFIG
