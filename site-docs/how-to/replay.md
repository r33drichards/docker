# Replay events and assert JavaScript state

Replay evaluates assertions inside the same JavaScript realm as the script. It can
inspect lexical Maps, closures and object identity without flattening them to JSON.
Use deterministic scenarios for ordering, then native integration for bridge behavior.

## Run the deterministic suites

~~~sh
node tests/replay/run.cjs
sh tests/js_replay.sh
~~~

The first uses Node's V8 realm. The second embeds the production `js_runtime::Runtime`
with matching V8 headers/libraries. Both still use a modeled host. Native transport,
permissions and lifetime correctness require the real-daemon tests below.

A trusted scenario can create state, replay events and assert the resulting heap:

~~~javascript
replay.configure({users: [{id: '001AAA', local: true, isOper: true}], channels: ['#feed']});
const counts = new Map();
ircd.on('postCommand', event => {
  counts.set(event.command, (counts.get(event.command) || 0) + 1);
});
replay.event('postCommand', {userId: '001AAA', command: 'PING', params: []});
replay.assert("counts instanceof Map && counts.get('PING') === 1");
~~~

`replay.advance(seconds)` controls time, runs due managed timers and emits
`backgroundTimer`. `replay.pending()` exposes request tokens; `replay.complete(token,
response)` controls their completion order. Inspect `replay.effects` and `replay.trace`
for ordering. Assertions must evaluate to exactly `true`.

Include failure cases: callback exceptions, later vetoes, cancelled or reordered
completions, stale targets, repeated events and restart reconstruction. Failed callbacks
discard modeled effects but retain JavaScript heap mutations. The model is deliberately
not a proof that native code has identical rollback or permission behavior.

## Test the actual daemon heap

`tests/native_host_api.py` uses `tests/replay/native.py` to drive real IRC events and
assert predeclared Map/closure expressions in the actual loaded script. Incoming IRC
text selects an assertion by index; it is never evaluated as arbitrary JavaScript.
There is no production debug/eval endpoint.

~~~sh
INSPIRCD_NATIVE=/path/to/disposable/installation sh tests/javascript.sh
~~~

This requires the matched daemon/module/probe build. Never run it against a live
server: the host suite intentionally tests rehash, self-unload and shutdown.

## Test applications without overstating coverage

RSS replay and SQLite restart tests live in the [separate RSS stack](../reference/features.md).
Run that checkout's documented commands; these files need not exist in this branch.
A SQL-contract mock is a deterministic model, not evidence that native database commits
survive a process restart. Real SQLite tests must cover commit/rollback, locks, reload
and restart. A local HTTP fixture proves the tested pipeline, not live public DNS/TLS
connectivity. A checkpoint placeholder is not a heap-snapshot implementation.

See [test setup](test.md) and the checked-in
[replay contract](https://github.com/r33drichards/docker/blob/feat/native-api-surface/tests/replay/README.md).
