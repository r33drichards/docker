# Optional JavaScript and WebAssembly command hooks

JavaScript module documentation: [Diátaxis documentation site](https://r33drichards.github.io/docker/) (MkDocs).
Source and local build instructions: [site-docs](site-docs/index.md), [documentation guide](site-docs/how-to/documentation.md).

`m_javascript` embeds V8 for **trusted administrator-authored scripts**. It is
separate from the unredacted `m_precommand` debug logger. Neither module is loaded
by default. JavaScript can reject/rewrite commands and request explicit native
state changes. This is not a hostile-code sandbox: V8/native out-of-memory errors
can terminate the IRC process, and blocking native code cannot be interrupted.

## Build and configure

```
docker build -t inspircd:local .
```

The primary image uses Alpine 3.22, InspIRCd v4.12.1 by default, and matching
`nodejs-dev`/`nodejs-libs` 22.23.2-r0. Only V8 APIs are used; no Node environment,
`process`, `require`, filesystem API, general networking API (`fetch`), or WASI is
installed in scripts. The explicit queued webhook API below is opt-in.
The root `Dockerfile` includes the compiled module and its runtime library, but
leaves it unloaded until explicitly configured. No scripts are installed or enabled
by default. `Dockerfile.javascript` has been retired; downstream Railway projects
can build the root Dockerfile and supply their own configuration and scripts.
Existing `/inspircd/conf/` volumes are preserved by the unchanged entrypoint.
Update `NODEJS_VERSION` for both stages together for security updates. The build
checks that the module links libnode and has the ELF NODELETE flag.

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

Register handlers in an administrator-configured script:

```
ircd.on('preCommand', event => {
  if (event.validated) return; // Rewrite only before native validation.
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
owned command/parameter snapshots first at `validated=false`, then at
`validated=true` immediately before execution if native validation succeeds.
The validated phase supports observation and pass/deny, never command/parameter
rewrites: native access and parameter checks have already completed. Post callbacks receive
`result` (`SUCCESS`, `FAILURE`, `INVALID`) and `loop`, matching the native hook.
`validated` is true for the post-handler snapshot. This does not prove delivery
to another client.

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
- `{ action: 'pass', command: 'PING', params: ['text'] }`: request a rewrite only at `validated=false`.

A validated-stage veto can inspect the final command without bypassing native permissions:

```js
ircd.on("preCommand", event => {
  if (event.validated && event.command === "VERSION")
    return {action: "deny"};
});
```

A callback receives both phases; guard `event.validated` when a queued notice or
webhook should be emitted only once. Unknown or rejected commands may never reach
the validated phase. Returning rewrite fields in that phase disables the script
and denies the command, even when the proposed value matches the current value.

Rewrite parameters may also be Uint8Array values. Rewrites require uppercase
ASCII command tokens, at most 15 parameters, a conservative 510-byte line limit,
no NUL/CR/LF, and valid middle-parameter syntax. Lone UTF-16 surrogates are rejected.
Unknown/server-only target handlers are rejected. Rewrites are redispatched under
the original client through the core parser, including operator/registration
checks, rather than invoking a cached handler. Tags are retained internally.
This incurs additional core flood accounting. The exact replacement skips a
second unvalidated JS pre pass, but its validated phase still runs; nested commands
still pass through hooks, with rewrite depth
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
- `postWebhook(name, json)` stages a bounded JSON POST to an administrator-configured endpoint (admin callbacks only).
- `webhookStats()` returns aggregate `{pending, sent, failed, discarded}` counters (admin scripts only).

Default `authority="user"` restricts notice/rename targets to the triggering
client. Rename runs ordinary NICK processing as that client, retaining native
restrictions. Explicit `authority="admin"` permits targeting other local users;
rename directly invokes the native administrative nickname change after nickname
validation. No credential, operator identity, or permission is granted to a user.
Queued operations re-resolve the original actor and target UUIDs and skip users
who disconnected. Only local targets are changed in this initial API.

At most 16 effects can be queued per file/event. All effects for the event are
discarded on pre denial/error. Accepted IRC effects run at the main-loop safe point,
after the current parser stack unwinds; accepted webhooks enter the owned worker
queue instead, after the complete JavaScript callback batch accepts the event. They may still be skipped/rejected by
native checks; queuing is not a success receipt or an atomic transaction, and a
later core command rejection does not retract accepted queued effects. Native
effects and commands they synchronously trigger do not invoke JS hooks, avoiding
unbounded feedback loops; ordinary core/native-module checks still apply.


### Opt-in channel join webhooks

Use **one** successful join observer, `postJoin`, not both `userJoin` and
`postJoin`. Its arguments are `event.memb.user` and `event.memb.channel`
(not `event.details.memb`); `user.id` is a plain UUID string, while nick/channel
names use HookText snapshots. Copy `examples/javascript/join-webhook.js` into your
config directory, edit its explicit channel allowlist, and load it **once**:

~~~xml
<module name="javascript">
<javascriptwebhook name="joins" url="https://hooks.example.invalid/irc">
<javascript file="scripts/join-webhook.js" authority="admin">
~~~

The URL is a placeholder: configure your own receiver and consented channels.
No endpoint, script or network worker is enabled by default. The example selects
local, non-quitting users to avoid reposting remote joins on every linked server,
skips invalid/truncated text, and includes only an event name, user UUID/nick and
channel. It does not export IP addresses, hosts, credentials or message bodies.
Channel allowlist matching uses the server snapshot's exact canonical spelling.
Each genuine later rejoin is a new event; this is not an exactly-once audit feed.
The receiver must accept this JSON schema; services with a different schema need
an administrator-authored payload adapter.

Only trusted admin-authority callbacks may call `ircd.postWebhook('joins',
JSON.stringify(object))`. Scripts choose a configured **name**, never a URL,
headers, method, file or credentials. Names are unique, case-sensitive ASCII
letters/digits/underscore/hyphen, 1–64 characters; at most 16 endpoints may be
configured. URLs are at most 2048 bytes and require HTTPS, with no userinfo,
fragment, spaces or control bytes. Credentials in a private URL must remain in
private server configuration. Neither URLs nor request/response bodies are logged.
For isolated tests or an explicitly trusted same-host relay only, an endpoint
may set `loopbackhttp="yes"` to allow HTTP at literal `127.0.0.1` or `[::1]`;
hostnames and non-loopback HTTP remain rejected. HTTPS still verifies peer and
hostname with the system CA trust store. Redirects, environment proxies and
application retries are disabled; no cookie jar or caller-supplied auth is used.

#### Endpoint-specific authentication headers

An administrator can attach headers to one named destination, with values read
from local secret files when configuration is loaded:

~~~xml
<javascriptwebhook name="nocodb" url="https://nocodb.example.invalid/api/v2/tables/TABLE_ID/records">
<javascriptwebhookheader endpoint="nocodb" name="xc-token" valuefile="/run/secrets/nocodb-token">
~~~

For Railway environments without secret-file mounts, the image provides a
separately opt-in [token-file startup helper](https://github.com/r33drichards/docker/blob/master/docs/nocodb-token-provisioning.md)
using `/inspircd/.secrets/nocodb-token`. It does not enable forwarding by itself.

The example is disabled unless explicitly configured. Use the API route and
column names required by your NocoDB installation; the join example's nested
payload is not automatically a NocoDB row. An administrator-authored script can
post a matching row object with `ircd.postWebhook('nocodb', JSON.stringify(row))`.
For bearer authentication, configure `name="Authorization"` and put the complete
`Bearer ...` value in its file. Never place credentials in scripts or URLs.

Files must be absolute paths, regular files owned by the daemon's effective UID,
with exactly mode `0400` or `0600`. All path components must be real directories
or files, without symlinks, `.` or `..`; mount secrets accordingly. Parent
directories must be owned by root or the daemon and not group/world-writable.
A root-owned sticky shared ancestor (such as `/tmp`) is accepted only when
immediately followed by a daemon-owned `0700` directory. Values are read verbatim:
1–1024 bytes, no NUL, CR, LF, tab, other ASCII control bytes or DEL. In particular,
do not end a token file with a newline. No trimming or interpolation occurs.
Missing, unreadable, changed-during-read, unsafe or invalid files reject the whole
candidate configuration, retaining the last-good generation on rehash.

The directive is repeatable: maximum 8 headers per endpoint, 64-byte HTTP token
names, and 8192 aggregate header bytes including names, values and `: ` / CRLF.
Names are case-insensitive; duplicates and unknown endpoints are errors.
Transport/control fields cannot be overridden: Host, Content-Type/Length/Encoding,
Transfer-Encoding, Accept-Encoding, Expect, Connection, Keep-Alive, TE, Trailer,
Upgrade, HTTP2-Settings, Cookie, Set-Cookie, proxy-* and method-override variants.
Headers belong only to their configured endpoint. Redirects remain disabled.

Scripts cannot set/read headers or open the files. Secret values and file paths
are omitted from loader diagnostics, logs, counters and the JavaScript API.
Immutable header storage is shared by the owning generation and its requests;
editing a file has no effect until a successful rehash. Rotation retires old
queued work and cancels old in-flight work under the same rules below; already
transmitted requests cannot be recalled. Secrets remain in process/libcurl memory
while owned; this does not guarantee zeroization or protection against process
memory access. This change supplies authentication only, with no new event hooks,
durable delivery, automatic retries or default credentials.

The payload must be a UTF-8 JSON **object string**, at most 16 KiB. Invalid arity,
types, JSON, destinations, excessive per-event effects, initialization calls and
user-authority calls are script errors under the existing failure policy.
`true` means **staged**, not sent: every request shares the event's 16-effect
budget and is discarded with the entire batch on denial, exception, timeout or
unsupported Promise return. A full queue returns `false` without blocking or
retrying. As with other effects, a later native veto cannot undo an already
accepted batch: use a successful observer such as `postJoin` for join receipts.

One worker owns copied requests; it never enters V8 or accesses native IRC objects.
A module-wide maximum of 64 reservations covers staged, queued **and in-flight**
payloads across all scripts/generations. Transfers have 1-second connect and
2-second total libcurl timeouts and discard at most 64 KiB of combined response
headers/body. Curl must support thread-safe global initialization and asynchronous
DNS; native build requires libcurl 7.85+ development headers, runtime library and
CA certificates. Deadlines are not a hard real-time scheduling guarantee.

A failed rehash retains the old generation and configured destinations, and
candidate initialization cannot send. A successful rehash retires old queued jobs
and requests cancellation of old in-flight transfers without retargeting them.
Unload clears queued work, requests cancellation, and joins the owned worker.
A request already transmitted cannot be recalled. There is no persistence or
automatic retry: timeout/cancellation/crash outcomes can be ambiguous, and HTTP
2xx means only that the receiver accepted the HTTP request, not that downstream
actions completed. Consumers needing delivery/idempotency must implement their
own stable event IDs, durable receiver and reconciliation.

`webhookStats()` reports reservations still alive (`pending`), completed 2xx
responses (`sent`), failed/ambiguous transfer attempts (`failed`), and queued
jobs discarded before an attempt (`discarded`). Pre-batch aborts simply release
their reservations. Counters reset on module unload, not rehash; no endpoint,
credential, body or response is exposed.

Local checks: `node tests/test_join_webhook.cjs` and
`python3 tests/test_js_webhooks.py` (C++17 compiler, pkg-config/libcurl, OpenSSL
CLI and Python required; no dependency-skip fallback). The latter creates only
owned loopback HTTP/TLS fixtures and checks bounds, cancellation, no redirects,
proxy suppression, untrusted certificate rejection and HTTP error/size/timeout
handling. The native daemon suite additionally exercises real JOIN-to-POST,
rejoin, authority, rollback and failed-rehash behavior.

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

The opt-in join webhook example uses only `postJoin` with `event.memb` snapshots;
see the Native API and authority section above.

`ircd.on(stage, callback)` supports all 75 `Module::On*` callbacks: the 74
Implementation-table events plus direct `OnCleanup`. See
[the per-hook inventory](bindings/module_hooks_coverage.md) and
`modules/js_generated/module_hooks.d.ts` for exact names, arguments, native
return semantics, and failure policy. The separate event-provider interfaces documented below are also bridged. Native
lifecycle methods (Init, ReadConfig, Prioritize, destruction) are not script events. Wasm can participate through these JS callbacks;
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

Void hooks observe only unless listed as mutable below; other return values are
ignored except unsupported Promises.
ModResult hooks accept undefined or `{action: "pass"}`, `{action: "deny"}`,
and (only with admin authority and where inventoried) `{action: "allow"}`.
Allow has the *native hook-specific meaning*, not a generic permit: consult the
inventory/upstream documentation. acceptConnection never accepts allow because
it would claim native socket ownership. Hooks run in native module order and
an earlier decision can prevent later modules/callbacks from observing an event.
Exceptions/timeouts/invalid decisions disable the script until successful rehash.
Decision hooks fail closed, except kill, channelPreDelete and userPreQuit pass
on failure so cleanup cannot be blocked by a disabled script. Observational
failures do not undo native actions. Only the explicit patches below write back
to native state; other mutable-reference arguments remain snapshots.

### Explicit native mutation patches

Return `{action: "pass", patch: {...}}`, rather than editing a snapshot. A patch
is staged and committed only when every invoked callback/file succeeds and none
returns deny. Unknown fields, wrong types, invalid values, or unauthorized fields
quarantine the script. Failed void-hook patches leave native state unchanged;
failed decision-hook patches also deny the operation. Queued effects are discarded.
Each callback still sees the original native snapshot; patches accumulate in a
separate staged copy, not in the event object. User-authority patches require a
local acting user. Administrator scripts remain trusted server policy code.

```js
ircd.on('userPreMessage', event => {
  if (event.details.text.text === 'hello bot')
    return {action: 'pass', patch: {text: 'Hello from JavaScript!'}};
});
ircd.on('userPart', event => {
  if (event.partmessage.text === 'rewrite reason')
    return {action: 'pass', patch: {partmessage: 'See you later'}};
});
ircd.on('buildNeighborList', event => {
  // Restrict notifications to the listed, already-included channel memberships.
  const channels = event.include.items
    .filter(m => m && m.channel && m.channel.name.text === '#public')
    .map(m => m.channel.name.text);
  return {action: 'pass', patch: {includeChannels: channels}};
});
```

Supported patch fields:

- `userPreMessage`: `text`, `exemptions: [userUuid]`,
  `tagsOut: [{name, value}]`, `echo`, `echoOriginal`, `updateIdle`.
- `userPart`: `partmessage`, `exemptions: [userUuid]`.
- `buildNeighborList`: `includeChannels: [channelName]`,
  `exceptions: [{userId: userUuid, include: false}]`.
- `userPreJoin`: administrator-only `privs`, a string of distinct registered
  prefix-mode letters (for example `"ov"`, not display symbols `"@+"`).
- `preChangeConnectClass`: administrator-only
  `errnum: {code: 499, params: ["Connection unavailable"]}` or `errnum: null`.
  This sets the error numeric for subsequent native handling; it does not itself
  deny a connection. A deny decision discards staged patches.

Strings reject NUL, CR, LF and unpaired UTF-16 surrogates. Message text is nonempty
and at most 400 UTF-8 bytes; PART reasons may be empty and have the same limit.
Arrays have at most 256 entries. User UUIDs must resolve to live registered users.
User-authority `exemptions` add recipients to the existing exclusion set;
administrator-authority `exemptions` replace that set. Neighbor channel lists
only filter existing memberships, even for administrators. User scripts may
only add `include: false` neighbor exceptions; administrators may use true for
live local users. No native pointer or membership can be fabricated.

Outgoing tag patches may only retain or remove existing tag names. User scripts
cannot change their values; administrators may change values, preserving the
original native provider and its delivery rules. Existing snapshot tag values
are already IRCv3-escaped wire text. Returning an identical value preserves it;
a changed administrative value is treated as decoded text and escaped once by
the native tag encoder (including spaces, semicolons, and backslashes).
Tag names are limited to 128
bytes, values to 512, and combined escaped tag-wire bytes to 4096. New tag providers or
arbitrary server tags cannot be created. Message flags are booleans. Privileges
are at most 64 bytes. Connection error codes must be 400–599 with 1–14 parameters
satisfying command-wire validation and its 510-byte aggregate bound.

Message destination identity/type remains immutable. Changing it after native
checks would bypass routing permissions. To reroute, rewrite `PRIVMSG` or
`NOTICE` in an unvalidated `preCommand` callback so the core validates the new
destination. For example:

```js
ircd.on('preCommand', event => {
  if (!event.validated && event.command === 'PRIVMSG' &&
      event.paramBytes.length === 2 && event.params[0] === '#old-room') {
    return {action: 'pass', command: 'PRIVMSG',
      params: ['#new-room', event.paramBytes[1]]};
  }
});
```

Inventory/ABI checks:

```sh
python3 tools/generate_module_hooks.py --check --upstream /path/to/inspircd/include/modules.h
python3 tests/test_generate_module_hooks.py
```

The synthetic native suite additionally requires the test-only `m_js_hook_probe`
module (optional image build arg `TEST_HOOK_PROBE=1`). It invokes all adapters,
checks snapshots after native stack destruction, exercises every ModResult
pass/deny/admin-allow decision, and sends real lifecycle/message traffic.
The normal primary image excludes this probe and remains unloaded by default.

## Separate event-provider hooks (InspIRCd 4.12.1)

These events supplement the generated core `Module` hooks. They register against
InspIRCd 4.12.1 event providers and become active when the provider is loaded;
loading JavaScript does not load services, CAP, HTTP, or spanning-tree for you.
Listeners survive provider unload/reload. Callbacks are synchronous and must not
return a Promise, including from an `async` function.

All payloads are detached, bounded copies. Assigning to them **does not modify
native state**. Text uses `HookText` (`text`, `bytes`, `byteLength`, `truncated`),
not a plain string. `text` may be null for invalid UTF-8. Each event also includes
`stage` and `userId` (null when there is no acting user). No socket, response
context, native object, or native pointer is exposed.

### Event names and fields

| Events | Additional payload fields |
| --- | --- |
| `accountChange` | `user`, `account`, `loggedIn`; empty account means logout |
| `userPreAway` | `user`, `message` |
| `userPreBack` | `user` |
| `userAway`, `userBack` | `user`, `previouslyAway`, `previousMessage`, `previousTime`, `away`, `message`, `time`; times are Unix seconds |
| `whois` | `source`, `target` |
| `whoisLine` | `source`, `target`, `numeric` |
| `whoLine` | `source`, `user`, `membership`, `numeric`, and WHO request fields below |
| `whoMatch` | `source`, `user`, and WHO request fields |
| `whoVisible` | `source`, `membership`, and WHO request fields |
| `namesListItem` | `issuer`, `membership`, `prefixes`, `nick` |
| `stats` | `source`, `symbol`; response rows are deliberately omitted |
| `capAddDel` | `name`, `adding` |
| `capValueChange` | `name`; values may be user-specific and are omitted |
| `serverLink`, `serverBurst` | server fields below |
| `serverSplit` | server fields and `error` |
| `syncUser` | server fields and `user` |
| `syncChannel` | server fields and `channel` |
| `syncNetwork` | server fields |
| `httpACLCheck`, `httpRequest` | `method`, `path`, `address` only |

WHO request fields: `matchText`, `matchChannel`, `fuzzyMatch`, `whox`, `queryType`.
Server fields: `serverId`, `serverName`, `serverDescription`. Server names and
identifiers are text snapshots, unlike the plain-string user snapshot `id`.

### Native decisions

- `userPreAway`, `userPreBack`, `whoLine`, `whoMatch`, `whoVisible` accept
  `{action: 'pass'}`, `{action: 'deny'}`, or `{action: 'allow'}`. `allow` requires
  an administrator-authority script because it can bypass later native policy.
- `namesListItem`, `whoisLine`, `stats` accept only pass or deny. For STATS, deny
  means "handled" and suppresses subsequent native handling; this bridge does
  not provide a response-row builder.
- Other events are observers. Return values do not claim native ownership or
  bypass permissions. In particular HTTP events always return native passthrough.
  A native HTTP deny would imply that the module already answered the socket;
  this bridge cannot do so. HTTP bodies, headers (including Authorization and
  cookies), query parameters, and fragments are never copied.
- Decision-hook failures fail closed; observer failures do not stop the native
  operation. Existing script quarantine and authority rules still apply.
- CAP events concern **server capability registration/value changes**, not an
  individual client's CAP REQ state. Use validated command hooks to observe
  client commands; do not infer successful CAP negotiation from the request.

Account events require a provider such as `services_account`; away, WHO, WHOIS,
NAMES and STATS use their core providers; CAP uses `cap`; server events use
`spanningtree`; HTTP uses `httpd`. A subscription without its provider is inert,
not an error.

### Example: refuse one away message and count logins

```js
// Load as a trusted script. No network calls or asynchronous callbacks.
let logins = 0;
const exactText = value => value && !value.truncated &&
  typeof value.text === 'string' ? value.text : null;

ircd.on('userPreAway', event => {
  if (exactText(event.message) === 'reserved test message')
    return {action: 'deny'};
});

ircd.on('accountChange', event => {
  if (event.loggedIn) ++logins;
});

ircd.on('serverSplit', event => {
  // Safe to retain: these objects contain copies, not live native handles.
  const name = exactText(event.serverName);
  if (name !== null) globalThis.lastObservedSplit = {name, error: event.error};
});
```

The counter and last split live only for this script generation; a successful
rehash replaces them. Retained snapshots do not authorize any later action.
