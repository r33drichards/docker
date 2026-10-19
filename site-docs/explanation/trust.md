# Why trusted scripts are not a sandbox

The module removes ambient Node APIs: scripts do not receive `process`, `require`,
filesystem/network APIs or WASI. That narrows the surface, but it does not isolate
the IRC daemon from an embedded native engine. V8 or native out-of-memory failure
can terminate the process; a watchdog cannot interrupt blocking native code.
The old-generation target is not a total process-memory ceiling.

Authority describes the bridge's permitted effects, not whether source code is
safe to run. User-authority code may affect the triggering local actor; explicitly
admin-authorized code may target other local users. An admin file's isolate does
not share globals with a lower-authority file. Neither mode is suitable for
unreviewed user-supplied programs.

JavaScript remains the host language and Wasm participates through explicit JS
wrappers. Wasm does not create a separate native hook ABI or hard containment.
A separate-process design would be needed for a stronger crash/resource boundary;
that redesign is not implemented here. Runtime checks and trusted-source review
remain necessary even when the script uses only Wasm exports.

The separate precommand debug logger is also not a security boundary. It may log
unredacted command data. This module's bridge avoids printing command contents
and script exception text, but script snapshots themselves can contain credentials
and private messages. Treat access to scripts and retained data as privileged.
