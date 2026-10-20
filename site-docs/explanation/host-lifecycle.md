# Host authority, effects and lifecycle

## Two authority levels

The administrator assigns `authority="user"` or `authority="admin"` when loading a
script. Code cannot promote itself by passing a boolean or a different user ID.
User scripts may observe allowed snapshots and use their restricted existing actions.
Admin scripts may use the wider host surface and are trusted like native modules.
The bridge is not a hostile-code security sandbox.

| Operation | When allowed | Result meaning |
| --- | --- | --- |
| Read-only admin queries | Initialization or callback | Owned point-in-time data; can already be stale when used |
| `registerCommand` | Admin initialization only | Candidate registration, installed after successful configuration load |
| Managed timer creation/cancellation | Admin initialization or active callback | Timer state change, not a durable reservation |
| Native mutations and administration | Admin active callback | `true` means staged, not executed or delivered |
| HTTP request creation | Admin active callback | Token identifies owned asynchronous work, not a response |
| Result polling/cancellation | Admin initialization or callback | Results are consumed; cancellation is best effort |

## Three different kinds of state

1. **JavaScript heap:** a `Map.set` or closure mutation happens in the realm. A later
   exception does not undo it.
2. **Staged host effects:** mutations, fetch commitments and managed timer changes
   follow their acceptance/rollback rules. Failed or vetoed callbacks discard staged
   effects. Timer changes are restored on rollback; heap changes are not.
3. **External effects:** an HTTP request already transmitted or an accepted database
   transaction cannot be recalled by pretending the event never happened.

Write tests for all three. Never use a queued return value as a delivery receipt or
assume database commit and IRC posting form one atomic transaction.

## Dispatch and revalidation

Native actions execute after JavaScript returns at a safe main-loop point. Arguments
are copied, actor and target identities are resolved again, and disappearing targets
are skipped. Script-owned effects carry generation identity. Native operations do not
recursively invoke the same JavaScript hooks; ordinary daemon checks still apply.

Accepted self-unload invalidates the dispatch state and generation before the deferred
destructor runs. Subsequent callback entries and later queued effects are rejected.
HTTP commits use this ordered action queue too. A request committed before self-unload
may already have transmitted data and cannot be undone.

## Reload is replacement, not a heap checkpoint

A candidate configuration is constructed before activation. Failed rehash retains the
last-good generation; successful replacement retires the old scripts, jobs, callbacks
and timers. Rebuild commands, timers and transient job state during initialization.
A poisoned/disabled script also retires its queued and in-flight HTTP jobs; already
transmitted requests are irreversible. No native timer or network handle should be serialized as application state.

The separate SQLite/RSS stack rebuilds transient Maps from explicitly stored data.
Its transactions are independent of IRC effect rollback. Persistence does not provide
exactly-once posting across a crash between database commit and channel transmission.
See the [branch matrix](../reference/features.md).

## Limits and failures

Host mutations accept strict arguments, use bounded queues, and reject malformed wire
text. Admin mutation string arguments are at most 400 UTF-8 bytes; per-file/event effects
are bounded. Query snapshots never expose secret channel mode parameters or native
pointers. Fetch requests have independent body, response, queue and network deadlines.
A deadline is not a hard real-time guarantee for OS scheduling or storage I/O.

Consult [execution limits](../reference/limits.md), [method declarations](../reference/callable-api.md)
and [fetch behavior](../how-to/fetch-and-webhooks.md) for the exact interface.
