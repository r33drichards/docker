# Native host API

This change exposes native server capabilities to **trusted admin-authority scripts**. It does not change script authority defaults. Ordinary user-authority scripts cannot register commands, inspect channel membership, create HTTP jobs/timers, or invoke these administration methods.

## Queries and commands

`userByNick`, `channelByName`, and `channels` return owned snapshots, never native pointers. Channel snapshots include topic, non-secret mode parameters, up to 4096 members with rank/prefix, and a truncation flag. Channel listing fails above 4096 results rather than silently truncating. Existing `userById` remains compatible.

`registerCommand(name, minParams, maxParams, operOnly, callback)` is initialization-only. Names use uppercase ASCII letters/digits and cannot replace existing native commands or another script's command. Registrations are replaced only after a successful configuration load. The callback gets `{userId, params}`; effects are discarded on an exception. All custom commands are local-only. Registration does not imply permission to grant operator privileges.

## Timers and I/O

`setTimer(seconds, repeat, callback)` accepts 1..86400 seconds and at most 32 timers per script. It runs on the existing background timer, on the main thread, with no user actor. Repeating timers skip missed intervals rather than catching up in a burst. `clearTimer(id)` cancels one. Timer mutations inside callbacks roll back if the callback throws or a later decision callback vetoes the event; initialization mutations take effect immediately. JavaScript heap mutations are not rolled back. Rehash/unload destroys timers. These native handles are not durable state and must be reconstructed after reload or process restart. This branch does not restore heap snapshots.

`fetch(url, {method: 'GET'|'POST', body})` stages a bounded asynchronous request. It is committed only after the enclosing callback succeeds, through the same ordered daemon action queue as native mutations. An accepted self-unload suppresses later fetches in that queue; requests committed before self-unload may already have transmitted data and cannot be undone. `takeFetchResult(id)` polls for an owned result with a byte-array body; `cancelFetch(id)` cancels/discards best-effort. Cancellation cannot undo transmitted POST data. Each script has at most 16 requests and two workers. Requests are capped at 64KiB, responses at 512KiB. Public HTTP(S) destinations only, standard ports, no redirects, proxy, URL credentials or arbitrary headers. Address checks run at socket creation, including resolved DNS addresses. No worker enters V8. There are no Promises/microtasks or standalone DNS resolver bindings in this interface.

## Channel and user operations

`join`, `part`, `kick`, `setTopic`, `setModes`, `setHost`, `setUsername`, `setRealname`, and `disconnect` stage owned operations. Targets are resolved again after the JavaScript callback returns. Missing/quitting targets are discarded. `join` requires a local user and preserves native join checks. `setHost`/`setUsername` change displayed identity; no credentials or native pointers are exposed.

## Administration

`addBan(type, mask, durationSecondsString, reason)` uses the installed native X-line factory; `removeBan(type, mask)` removes a line. `loadModule`, `unloadModule`, `rehash`, and `shutdown` expose native administration to admin-authority scripts. Module names cannot contain paths. These operations are **powerful and potentially destructive**: admin scripts are equivalent to trusted server modules, not a security sandbox. Successful self-unload invalidates its generation immediately, discarding subsequent queued effects before the daemon unload completes. They are deferred until V8 has returned; they do not execute from inside its call stack. No separate new privileges are enabled by loading this code.

A `true` result means **queued**, not successful completion. Native validation, module dependencies, unloaded targets, configuration failures or permissions can prevent the action. Never use return values as delivery receipts. Tests must exercise shutdown and unload only in disposable daemons.

## Integration status

The native API and webhook/docs review stack is separate from SQLite PR #10 and RSS PR #8. See the [feature matrix](../reference/features.md) for branch boundaries and merge-order conflicts. Reload does not persist timers, sockets or command handles; native resources must be re-created without duplicate registrations.

The bounded surface is not raw C++ SDK parity: standalone DNS resolution, user-mode mutation and real (rather than displayed) host/username mutation remain unexposed. Paired channel `privmsg`/`notice` routing is implemented separately in RSS PR #8; this PR does not silently depend on it. JavaScript cannot supply arbitrary HTTP headers. Host-configured header injection is described in the [fetch guide](fetch-and-webhooks.md); redirects and browser Promise compatibility remain unsupported.

## Replay tests

`node tests/replay/run.cjs` and `sh tests/js_replay.sh` exercise deterministic event sequences and actual JavaScript heap Maps/closures. The latter embeds the production V8 runtime; its host is still a model, not a replacement for native bridge testing. `tests/native_host_api.py` additionally drives a disposable real daemon, including the replay adapter in `tests/replay/native.py`, timer rollback, rehash, self-unload and shutdown. See [the replay harness notes](https://github.com/r33drichards/docker/blob/feat/native-api-surface/tests/replay/README.md) for model limits.
