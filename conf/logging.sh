#!/bin/sh
# Normal-level diagnostics only: raw IRC and debug authentication traces are excluded.
case ${INSPIRCD_PERSISTENT:-no} in
    no)
        printf '%s\n' '<log method="file" level="normal" type="* -USERINPUT -USEROUTPUT" target="inspircd.log">'
        ;;
    yes)
        printf '%s\n' '<module name="log_bounded">' '<log method="bounded" level="normal" type="* -USERINPUT -USEROUTPUT" maxbytes="10485760" files="5">'
        ;;
    *) printf '%s\n' '<die reason="INSPIRCD_PERSISTENT must be yes or no">'; exit 1 ;;
esac
