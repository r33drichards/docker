# Optional NocoDB token file provisioning

The default Docker image does not enable JavaScript, scripts, or forwarding.
It only materializes a token when `INSPIRCD_NOCODB_TOKEN` is explicitly set.
An absent variable leaves startup behavior and files unchanged; an empty or
invalid value fails startup without printing its value.

After the administrator approves the actual NocoDB credential scope, they must
enter and seal this variable themselves in the Railway **docker** service's
**production** variables UI. Do not paste a token into chat, source code,
configuration text, a start command, command arguments, or build arguments.
Railway sealed variables remain available to builds and deployments, even though
UI/API readback is blocked. Do not add an `ARG`, `ENV`, build command, or logging
that consumes this value.

The startup helper runs under the image's existing UID/GID 10000. Before any child
process, it disables shell tracing, copies the token into a nonexported shell
variable, and unsets `INSPIRCD_NOCODB_TOKEN`. It writes the exact token bytes
without a trailing newline to:

`/inspircd/.secrets/nocodb-token`

The containing directory is mode 0700 and the new regular file mode 0600. Existing
symlinks, unsafe permissions/owners, nonregular files, and hardlinked files are
rejected rather than repaired or overwritten. Valid file rotation uses a new
exclusive temporary file and atomic replacement in the private directory.
Only nonempty visible ASCII token bytes (no whitespace), at most 1024 bytes, are
accepted. The daemon then starts through the existing entrypoint with the token
variable removed from its inherited environment.

The initial startup process still receives the Railway environment secret;
this is not protection against a privileged host or a compromised process
running as the same UID. The file remains available to the authorized daemon.
No persistent volume is required: the file is recreated on each container start.
No pre-deploy command can supply this file, since that command runs in a separate
container. Removing the variable does not delete an existing runtime file; also
remove the endpoint/load configuration when disabling forwarding, and start a
fresh container. Credential revocation is performed in NocoDB when appropriate.

This helper alone does not load any script or configure a destination. Deployment
and forwarding activation require separately reviewed endpoint, observer, and
scope settings. Tests use fake credentials only:

`python3 tests/test_materialize_nocodb_token.py`

Tests run against an isolated copy with only the hard-coded root path substituted;
the production helper has no path override. Container integration must also check
actual UID 10000 ownership and the Alpine shell/runtime before release.
