#!/bin/sh
# Existing paths point into the one private volume after the bootstrap has validated it.
case ${INSPIRCD_PERSISTENT:-no} in
    no) exit 0 ;;
    yes) ;;
    *) printf '%s\n' '<die reason="INSPIRCD_PERSISTENT must be yes or no">'; exit 1 ;;
esac
cat <<'CONFIG'
<xlinedb filename="/inspircd/data/xline.db" saveperiod="5s" backoff="2" maxbackoff="5m">
<module name="permchannels">
<permchanneldb filename="/inspircd/data/permchannels.conf" listmodes="yes" saveperiod="5s" backoff="2" maxbackoff="5m" operonly="yes">
<include file="/inspircd/data/permchannels.conf" missingokay="yes">
CONFIG
