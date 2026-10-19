# Reload scripts and recover from failure

Use this guide after modifying an already configured script set. You need the
normal administrative access required to rehash your InspIRCd server.

1. Save the changed files and validate their syntax and intended effects in a
   disposable server. Initialization must register handlers without native mutations.
2. Request the server's normal rehash using your existing operator procedure.
3. Check the `m_javascript` configuration/runtime log and exercise the affected hook.
   A successful candidate replaces every script isolate, resetting global state.
4. If initialization fails, repair the candidate configuration/source and rehash
   again. The previous active set and its existing state remain active.
5. If a running file throws, times out, returns a Promise or an invalid decision,
   fix its source and perform a successful rehash to re-enable it. Do not assume
   a disabled decision hook silently permits traffic; consult the
   [failure rules](../reference/limits.md).

Do not replace a loaded module binary or V8 library in place. Restart the daemon
when changing those binaries. Module unload/reload of the same binary is supported,
but is not a filesystem watcher or code deployment mechanism.
