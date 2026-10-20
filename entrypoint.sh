#!/bin/sh
# shellcheck disable=SC2068

# Clear the optional token environment before any child command or daemon.
# shellcheck source=tools/materialize-nocodb-token.sh
. /materialize-nocodb-token.sh || exit 1

INSPIRCD_ROOT="/inspircd"

# Validate even when persistence is disabled, so import can never bypass its gate.
case "${INSPIRCD_PERSISTENT_IMPORT:-no}" in
    no|yes) ;;
    *) printf '%s\n' 'Persistent configuration: invalid import setting.' >&2; exit 1 ;;
esac
if [ "${INSPIRCD_PERSISTENT_IMPORT:-no}" = yes ] && [ "${INSPIRCD_PERSISTENT:-no}" != yes ]; then
    printf '%s\n' 'Persistent configuration: import requires persistent mode.' >&2
    exit 1
fi

# The opt-in persistent tree was validated before privileges were dropped.
# Seed/update only checksum-managed defaults; never overwrite user edits.
if [ "${INSPIRCD_PERSISTENT:-no}" = yes ]; then
    # PID 1 shells do not automatically forward a stop to a waiting child.
    _seed_pid=
    trap 'if [ -n "$_seed_pid" ]; then kill -TERM "$_seed_pid" 2>/dev/null; wait "$_seed_pid" 2>/dev/null; fi; exit 1' TERM INT
    /seed-persistent-config &
    _seed_pid=$!
    wait "$_seed_pid"
    _seed_status=$?
    trap - TERM INT
    unset _seed_pid
    [ "$_seed_status" -eq 0 ] || exit 1
    unset _seed_status
# Legacy, non-persistent configuration behavior remains unchanged.
elif [ ! -e $INSPIRCD_ROOT/conf/inspircd.conf ] && [ -w $INSPIRCD_ROOT/conf/ ]; then
    # shellcheck disable=SC2336
    cp -r /conf/* $INSPIRCD_ROOT/conf/
elif [ ! -w $INSPIRCD_ROOT/conf/ ]; then
    echo "
        ##################################
        ###                            ###
        ###   Can't write to volume!   ###
        ###    Please change owner     ###
        ###        to uid 10000        ###
        ###                            ###
        ##################################
    "
fi

# Link certificates from secrets
# See https://docs.docker.com/engine/swarm/secrets/
if [ -e /run/secrets/inspircd.key ] && [ -e /run/secrets/inspircd.crt ]; then
    ln -s /run/secrets/inspircd.key $INSPIRCD_ROOT/conf/key.pem
    ln -s /run/secrets/inspircd.crt $INSPIRCD_ROOT/conf/cert.pem
fi

# Make sure there is a certificate or generate a new one
if [ ! -e $INSPIRCD_ROOT/conf/cert.pem ] && [ ! -e $INSPIRCD_ROOT/conf/key.pem ]; then
    cat > /tmp/cert.template <<EOF
cn              = "${INSP_TLS_CN:-irc.example.com}"
email           = "${INSP_TLS_MAIL:-nomail@irc.example.com}"
unit            = "${INSP_TLS_UNIT:-Example Server Admins}"
organization    = "${INSP_TLS_ORG:-Example IRC Network}"
locality        = "${INSP_TLS_LOC:-Example City}"
state           = "${INSP_TLS_STATE:-Example State}"
country         = "${INSP_TLS_COUNTRY:-XZ}"
expiration_days = ${INSP_TLS_DURATION:-365}
tls_www_client
tls_www_server
signing_key
encryption_key
cert_signing_key
crl_signing_key
code_signing_key
ocsp_signing_key
time_stamping_key
EOF
    /usr/bin/certtool --generate-privkey --bits 4096 --sec-param normal --outfile $INSPIRCD_ROOT/conf/key.pem
    /usr/bin/certtool --generate-self-signed --load-privkey $INSPIRCD_ROOT/conf/key.pem --outfile $INSPIRCD_ROOT/conf/cert.pem --template /tmp/cert.template
    rm /tmp/cert.template
fi

# Make sure dhparams are present
if [ ! -e $INSPIRCD_ROOT/conf/dhparams.pem ]; then
    /usr/bin/certtool --generate-dh-params --sec-param normal --outfile $INSPIRCD_ROOT/conf/dhparams.pem
fi

cd $INSPIRCD_ROOT
exec env INSPIRCD_ROOT=$INSPIRCD_ROOT $INSPIRCD_ROOT/bin/inspircd --nofork $@
