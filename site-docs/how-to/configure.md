# Build and configure the module

Use this guide when adding trusted scripts to an existing InspIRCd 4.12.1 setup.
Review [trust and containment](../explanation/trust.md) before enabling it.

1. Build the optional image from the repository root:

   ~~~sh
   docker build -f Dockerfile.javascript -t inspircd:javascript .
   ~~~

2. Make your script files readable by the daemon user. Put them under your
   configuration directory, for example `scripts/hooks.js`. Do not give an IRC
   user write access to administrator-loaded scripts.
3. Add explicit configuration:

   ~~~text
   <module name="javascript">
   <javascriptsettings timeoutms="25">
   <javascript file="scripts/hooks.js" authority="user">
   <log method="stdout" level="normal" type="m_javascript">
   ~~~

4. Start a disposable instance with your complete, validated server configuration
   before changing a live server. In the image the config directory is
   `/inspircd/conf/` and the daemon UID/GID is 10000.
5. Check startup logs for script load errors and exercise the intended hook using
   synthetic data. Do not enable the unredacted precommand debug logger to debug secrets.

For multiple files, use repeated `<javascript file="...">` tags or a
`<javascript directory="scripts/extra" authority="user">` entry. Directory loading
is non-recursive; sort order, failures and size limits are in the
[configuration reference](../reference/configuration.md).
Grant `authority="admin"` only to reviewed files needing local cross-user effects;
never use it merely to suppress an authorization error.
