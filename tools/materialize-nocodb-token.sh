#!/bin/sh
# Source this before starting child processes. Never execute with a token argument.
# The default image has no token and this helper makes no changes in that case.
if [ "${INSPIRCD_NOCODB_TOKEN+x}" = x ]; then
    # Stop tracing before expanding the value, including when called from sh -x.
    set +x
    unset _noco_token
    _noco_token=$INSPIRCD_NOCODB_TOKEN
    unset INSPIRCD_NOCODB_TOKEN

    _noco_materialize() {
        # NocoDB tokens are nonempty visible ASCII; forbid whitespace/control bytes.
        case $_noco_token in
            ''|*[![:graph:]]*) return 1 ;;
        esac
        [ "${#_noco_token}" -le 1024 ] || return 1
        _noco_uid=$(id -u) || return 1
        _noco_root=/inspircd
        _noco_dir=$_noco_root/.secrets
        _noco_file=$_noco_dir/nocodb-token
        # Never follow a substituted parent or write beneath a writable parent.
        [ ! -L "$_noco_root" ] && [ -d "$_noco_root" ] || return 1
        _noco_owner=$(stat -c %u "$_noco_root") || return 1
        [ "$_noco_owner" = 0 ] || [ "$_noco_owner" = "$_noco_uid" ] || return 1
        _noco_mode=$(stat -c %a "$_noco_root") || return 1
        [ "$((0$_noco_mode & 0022))" -eq 0 ] || return 1
        if [ ! -e "$_noco_dir" ] && [ ! -L "$_noco_dir" ]; then
            mkdir -m 700 "$_noco_dir" || return 1
        fi
        [ ! -L "$_noco_dir" ] && [ -d "$_noco_dir" ] || return 1
        [ "$(stat -c %u "$_noco_dir")" = "$_noco_uid" ] || return 1
        [ "$(stat -c %a "$_noco_dir")" = 700 ] || return 1
        if [ -e "$_noco_file" ] || [ -L "$_noco_file" ]; then
            [ ! -L "$_noco_file" ] && [ -f "$_noco_file" ] || return 1
            [ "$(stat -c %u "$_noco_file")" = "$_noco_uid" ] || return 1
            [ "$(stat -c %h "$_noco_file")" = 1 ] || return 1
            case $(stat -c %a "$_noco_file") in 400|600) ;; *) return 1 ;; esac
        fi
        # mktemp creates a new regular0600 file; private parent prevents symlink races.
        _noco_tmp=$(umask 077; mktemp "$_noco_dir/.nocodb-token.XXXXXXXXXX") || return 1
        # printf is a shell builtin: the secret never enters an external command argv.
        if ! printf '%s' "$_noco_token" > "$_noco_tmp"; then
            rm -f "$_noco_tmp"
            return 1
        fi
        if ! mv -f "$_noco_tmp" "$_noco_file"; then
            rm -f "$_noco_tmp"
            return 1
        fi
    }
    # C locale makes the validation ASCII-only, independent of deployment locale.
    _noco_saved_lc=${LC_ALL-}
    _noco_had_lc=${LC_ALL+x}
    LC_ALL=C
    export LC_ALL
    _noco_status=0
    _noco_materialize || _noco_status=1
    unset _noco_token
    if [ "$_noco_had_lc" = x ]; then LC_ALL=$_noco_saved_lc; else unset LC_ALL; fi
    unset _noco_saved_lc _noco_had_lc _noco_uid _noco_root _noco_dir _noco_file _noco_owner _noco_mode _noco_tmp
    unset -f _noco_materialize
    if [ "$_noco_status" -ne 0 ]; then
        unset _noco_status
        printf '%s\n' 'NocoDB token provisioning failed; refusing startup.' >&2
        return 1
    fi
    unset _noco_status
fi
