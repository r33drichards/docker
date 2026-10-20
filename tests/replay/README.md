# JavaScript heap replay

This framework replays ordered events and evaluates assertions in the script's
own JavaScript realm. Maps, closures, object identity and lexical variables are
not flattened into JSON snapshots.

## Deterministic simulation

Run `node tests/replay/run.cjs`, or `tests/js_replay.sh` with matched V8 development
headers/libraries. Both run the same fixtures. The latter uses the actual
`js_runtime::Runtime` embedding, not Node's runtime. Each scenario starts in a
fresh isolate/context and has a one-second evaluation deadline.

Write trusted scenarios in `tests/replay/scenarios/*.js`:

```js
replay.configure({users: [{id: '001AAA', local: true, isOper: true}], channels: ['#feed']});
const counts = new Map();
ircd.on('postCommand', e => counts.set(e.command, (counts.get(e.command) || 0) + 1));
replay.event('postCommand', {userId: '001AAA', command: 'RSS', params: []});
replay.assert("counts instanceof Map && counts.get('RSS') === 1");
```

`replay.advance(seconds)` moves a monotonic fake clock, runs due managed timers
without catch-up bursts, and delivers `backgroundTimer`. `replay.pending()`
returns fetch tokens; `replay.complete(token, response)` controls completion
order. Cancelled/unknown tokens cannot complete. Inspect `replay.effects` and
`replay.trace` for messages and event/fetch ordering. `replay.assert(expression)`
requires the expression to evaluate to exactly `true` inside the realm.

The host is a **test model**, not production permission or transport code. It
models deferred message effects and target revalidation. Failed/vetoed callbacks
discard host effects, but **do not roll back JavaScript heap mutations**. An
exception disables further simulated dispatch until a fresh scenario. This
important distinction is tested. Native integration must independently verify
bridge permissions, wire byte limits, networking, callback lifecycle and routing.
The model does not make real network requests or invoke administration methods.

## Replay the real RSS script

After generic fetch/RSS is present, run:

```
node tests/replay/run.cjs --script examples/rss.js tests/replay/rss.js
tests/js_replay.sh --script examples/rss.js tests/replay/rss.js
```

This loads the actual RSS implementation before the scenario, then asserts its
lexical subscription Maps, seen IDs, retry deadlines, cancellation and per-channel
isolation using real RSS XML bodies. The scenario also reorders fetch completions.
It tests the script and heap, not the real HTTP transport.

## Native IRC replay

`tests/replay/native.py` provides `NativeReplay` and `exercise_heap_replay` for the
existing isolated-daemon integration harness. It drives real IRC commands and
adds predeclared assertions to the loaded script. A test PING selects an assertion
by index; incoming text is never evaluated as JavaScript. Assertions can access
lexical Maps and closures in the actual embedded V8 heap. No production debug or
eval endpoint is introduced.

```python
from replay.native import exercise_heap_replay
exercise_heap_replay(caller, reload_script)
```

Native replay uses real daemon time/networking; deterministic simulation uses a
fake clock and responses. Passing one does not establish correctness of the other.

## Snapshot integration

`replay.checkpoint()` intentionally fails until a real snapshot adapter is wired
in. There is no JSON save/restore substitute. Restart/closure/Map persistence must
be tested against the snapshot branch before claiming persistence works.
