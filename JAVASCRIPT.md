# Optional JavaScript and WebAssembly command hooks

`m_javascript` embeds V8 for **trusted administrator-authored scripts**. It is
separate from the unredacted `m_precommand` debug logger. Neither module is loaded
by default. JavaScript can reject/rewrite commands and request explicit native
state changes. This is not a hostile-code sandbox: V8/native out-of-memory errors
can terminate the IRC process, and blocking native code cannot be interrupted.

## Build and configure

```
docker build -f Dockerfile.javascript -t inspircd:javascript .
```

The optional image uses Alpine 3.22, InspIRCd v4.12.1 by default, and matching
`nodejs-dev`/`nodejs-libs` 22.23.2-r0. Only V8 APIs are used; no Node environment,
`process`, `require`, filesystem API, network API, or WASI is installed in scripts.
The original Dockerfile stays unchanged and does not enable the extra module.
Update both dependency pins together for security updates. The optional Dockerfile
checks that the built module links libnode and has the ELF NODELETE flag.

Example configuration (files must already exist):

```
<module name="javascript">
<javascriptsettings timeoutms="25">
<javascript file="scripts/hooks.js">
<javascript directory="scripts/extra" authority="user">
<javascript file="scripts/operator.js" authority="admin">
<javascriptwasm name="answer" file="scripts/answer.wasm">
<log method="stdout" level="normal" type="m_javascript">
```

Relative paths resolve under InspIRCd's configured config directory. Source
entries execute in configuration order; direct regular `.js` files inside each
directory execute in bytewise filename order. Subdirectories and discovered
symlinks are skipped. Explicit file/directory symlinks are canonicalized.
Duplicate canonical source paths, missing/unreadable files, empty matching
directories, and oversized sources fail configuration. Defaults are 128 sources,
1 MiB per source, and 16 MiB total; Wasm assets have the same separate limits.
Wasm asset names are unique case-sensitive ASCII letters/digits/underscore/hyphen,
1–64 characters; only explicitly configured `.wasm` files are loaded.

Each JavaScript file has its own isolate, global state, and configured authority.
Directory authority is inherited by every discovered file. Scripts cannot select
or change their authority, share globals with more privileged files, or choose a
native execution actor. Scripts are loaded as classic synchronous scripts, not ES
modules; no implicit imports, recursive includes, filesystem watching, or timers.

Normal InspIRCd rehash loads a complete candidate set with fresh isolates. It
replaces the active set only after every file initializes successfully. A failed
candidate leaves the previous scripts and their state active. Native mutations
are prohibited during initialization. Filesystem reads are bounded but are not a
filesystem-wide atomic snapshot. Successful reload resets all JavaScript state.

## Hooks and owned data

See `examples/javascript/hooks.js` and `modules/js_api.d.ts`.

```
ircd.on('preCommand', event => {
  if (event.command === 'EXAMPLEPING')
    return { action: 'pass', command: 'PING', params: ['hello'] };
});
ircd.on('postCommand', event => {
  if (event.command === 'VERSION')
    ircd.notice(event.userId, 'Handler result: ' + event.result);
});
```

Register at most 16 callbacks per stage per file during initialization. Callbacks
run synchronously in registration order, then file order. Pre callbacks receive
owned command/parameter snapshots at `validated=false`. Post callbacks receive
`result` (`SUCCESS`, `FAILURE`, `INVALID`) and `loop`, matching the native hook.
`validated` is true for the post-handler snapshot. This does not prove delivery
to another client. The earlier debug logger still observes validated pre events.

Fields are `command`, `params`, `commandBytes`, `paramBytes`, `userId`, `stage`,
`validated`, `result`, and `loop`. Valid UTF-8 text becomes JavaScript strings;
invalid UTF-8 becomes `null` in the corresponding text field. Uint8Array fields
always preserve exact owned bytes, including invalid UTF-8. Tags, client identity
fields, and native pointers are not exposed. Editing an event does not edit IRC
state. Snapshot values are unredacted; scripts must handle credentials/private
messages appropriately. The bridge itself does not print command contents or
script exception text. The separate debug logger does print unredacted contents.

Pre return values:

- `undefined` or `{ action: 'pass' }`: continue ordinary core processing.
- `{ action: 'deny' }`: stop processing and discard this event's queued effects.
- `{ action: 'pass', command: 'PING', params: ['text'] }`: request a rewrite.

Rewrite parameters may also be Uint8Array values. Rewrites require uppercase
ASCII command tokens, at most 15 parameters, a conservative 510-byte line limit,
no NUL/CR/LF, and valid middle-parameter syntax. Lone UTF-16 surrogates are rejected.
Unknown/server-only target handlers are rejected. Rewrites are redispatched under
the original client through the core parser, including operator/registration
checks, rather than invoking a cached handler. Tags are retained internally.
This incurs additional core flood accounting. The exact replacement skips a
second JS pre pass; nested commands still pass through hooks, with rewrite depth
bounded to eight. A later script sees earlier scripts' proposed changes.

Other modules may veto before this hook, and unknown commands can reach native
pre hooks more than once when another module resolves them. Core-generated
commands and per-target post events may also appear. There is no exactly-once
or complete-audit guarantee. Post return values are ignored except that Promises
are rejected. A post error cannot undo a command that has already executed.

## Failure and execution limits

Script initialization and each file's stage callback batch share its configured
wall-clock deadline (default 25 ms, configurable 1–1000 ms). A joined watchdog
terminates JavaScript/Wasm execution. A pre exception, timeout, invalid decision,
or Promise result rejects the command and disables that file until a successful
rehash. Subsequent events needing its callbacks fail closed. A failed post-only
file does not block commands through an empty pre stage. Post failures discard
that event's queued effects and are reported without script-controlled data.
Already-committed effects from earlier events are not rolled back.

There are no automatic microtask checkpoints; asynchronous hooks, Promises, and
async Wasm compilation are unsupported. `Atomics.wait` is disabled. Use
`new WebAssembly.Module(bytes)` and `new WebAssembly.Instance(module, imports)`.
Synchronous Wasm compilation/calls run within the same deadline; traps are normal
script errors. Each isolate has a 32 MiB old-generation target, not a hard total
memory cap. ArrayBuffers/native allocations and process-wide V8 resources are
outside it. Total work and memory grow with configured files. Use a small source
set; a separate process would be needed for hard resource/crash containment.

V8's platform is process-lifetime and never globally disposed/reinitialized on
rehash or module reload. ELF NODELETE retains callback/platform code; musl also
does not unload DSOs. Per-script handles, watchdogs, contexts, and isolates are
cleaned up. Reloading the same module is supported; replacing the module binary
or V8 libraries requires restarting the daemon. This module assumes sole
ownership of V8 initialization in the process.

## Native API and authority

The manifest `bindings/inspircd.json` generates registration rows, utility
TypeScript declarations, and a coverage inventory:

```
python3 tools/generate_js_bindings.py
python3 tools/generate_js_bindings.py --check
```

Six audited utility wrappers are implemented: `match`, `matchCIDR`, `isNick`,
`isChannel`, `stripColor`, and `time`. Inputs have strict arity/types, copied
ownership, and explicit conversions. NUL is rejected for native glob/CIDR
functions with NUL-terminated internals. The manifest tracks reviewed declarations,
manual-wrapper candidates, exclusions, and pending mutations. It is a systematic
allowlist and coverage inventory, **not an automatic binding of every function in
inspircd.h or the whole SDK**. New bindings require explicit conversion, lifetime,
effect, and authorization policy plus an implementation and tests.

Additional explicit host methods:

- `userById(uuid)` returns an owned `{ id, nick }` snapshot or null.
- `notice(targetUuid, text)` queues a server notice (maximum 400 UTF-8 bytes).
- `rename(targetUuid, nick)` queues a validated nickname change.
- `wasm(name)` returns a fresh Uint8Array of a configured asset's bytes.

Default `authority="user"` restricts notice/rename targets to the triggering
client. Rename runs ordinary NICK processing as that client, retaining native
restrictions. Explicit `authority="admin"` permits targeting other local users;
rename directly invokes the native administrative nickname change after nickname
validation. No credential, operator identity, or permission is granted to a user.
Queued operations re-resolve the original actor and target UUIDs and skip users
who disconnected. Only local targets are changed in this initial API.

At most 16 effects can be queued per file/event. All effects for the event are
discarded on pre denial/error. Accepted effects run at the main-loop safe point,
after the current parser stack unwinds. They may still be skipped/rejected by
native checks; queuing is not a success receipt or an atomic transaction, and a
later core command rejection does not retract accepted queued effects. Native
effects and commands they synchronously trigger do not invoke JS hooks, avoiding
unbounded feedback loops; ordinary core/native-module checks still apply.

## WebAssembly example and tests

`examples/javascript/answer.wasm` is the hand-encoded binary of the adjacent
`answer.wat`, exporting an i32 function returning 42. `wasm.js` demonstrates local
asset loading and a JS wrapper for command callbacks. There is no implicit bare
Wasm IRC ABI; imports must be supplied explicitly by that script and host calls
inherit the script's configured authority. No WASI or arbitrary file/network
access is supplied.

Run loader/generator tests with `sh tests/js_script_loader.sh` and
`sh tests/js_bindings.sh`. `sh tests/js_runtime.sh` uses matching installed V8
headers/libraries or explicit `V8_CXXFLAGS`/`V8_LDFLAGS`; it reports SKIP if absent.
Native daemon integration: `INSPIRCD_NATIVE=/path/to/run sh tests/javascript.sh`
after building/installing `m_javascript.so`. All traffic/data is synthetic and
loopback-only. The native suite does not modify an existing server configuration.

## Continuous integration

See [CI coverage and label-triggered heavy runs](docs/CI.md).

## All core Module event hooks (InspIRCd 4.12.1)

`ircd.on(stage, callback)` supports all 75 `Module::On*` callbacks: the 74
Implementation-table events plus direct `OnCleanup`. See
[the per-hook inventory](bindings/module_hooks_coverage.md) and
`modules/js_generated/module_hooks.d.ts` for exact names, arguments, native
return semantics, and failure policy. Optional modules’ separate EventProvider
APIs and native lifecycle methods (Init, ReadConfig, Prioritize, destruction)
are not additional script events. Wasm can participate through these JS callbacks;
there is no separate Wasm hook ABI.

```js
ircd.on("userPreMessage", event => {
  if (event.details.text.text === "blocked") return {action: "deny"};
});
```

Except for the existing specialized pre/post-command shape, arguments are owned,
bounded snapshots. Strings carry `{text, bytes, byteLength, truncated}`; invalid
UTF-8 has null text and intact bytes. Lists carry `{items, length, truncated}`.
Snapshots expose no native handles and changing them never changes native state.
Text is capped at 64 KiB per value and 256 KiB per event; lists at 256 entries.
Each event has `stage` and nullable `userId`; server events may have no actor.
User-authority effects require a local actor; explicit admin authority permits
local-target effects without an actor. The same queued-effect/reentrancy rules
apply to all events.

Void hooks observe only; return values are ignored except unsupported Promises.
ModResult hooks accept undefined or `{action: "pass"}`, `{action: "deny"}`,
and (only with admin authority and where inventoried) `{action: "allow"}`.
Allow has the *native hook-specific meaning*, not a generic permit: consult the
inventory/upstream documentation. acceptConnection never accepts allow because
it would claim native socket ownership. Hooks run in native module order and
an earlier decision can prevent later modules/callbacks from observing an event.
Exceptions/timeouts/invalid decisions disable the script until successful rehash.
Decision hooks fail closed, except kill, channelPreDelete and userPreQuit pass
on failure so cleanup cannot be blocked by a disabled script. Observational
failures do not undo native actions. No native mutation support is implied for
mutable-reference arguments outside the existing command rewrite API.

Inventory/ABI checks:

```sh
python3 tools/generate_module_hooks.py --check --upstream /path/to/inspircd/include/modules.h
python3 tests/test_generate_module_hooks.py
```

The synthetic native suite additionally requires the test-only `m_js_hook_probe`
module (optional image build arg `TEST_HOOK_PROBE=1`). It invokes all adapters,
checks snapshots after native stack destruction, exercises every ModResult
pass/deny/admin-allow decision, and sends real lifecycle/message traffic.
The normal optional image excludes this probe and remains unloaded by default.
