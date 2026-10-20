# Why snapshots, native ordering and deferred effects matter

IRC objects can disappear as a native callback unwinds. The bridge copies values
into owned, bounded snapshots so a retained JavaScript event does not become a
dangling native pointer. Text carries both a decoded view and exact bytes; truncation
metadata tells the caller when a bounded snapshot is incomplete. Editing a snapshot
changes the copy, not the IRC object. Command rewriting is a separately validated API.
Selected hooks also accept explicit bounded patches: these update staged copies
and commit only after all invoked callbacks succeed without denying the event.
A later native module can still reject the operation; patches are not a transaction
across the whole server.

Native module order still applies. A prior module's decision may prevent another
observer from seeing an event, and per-target/core-generated command callbacks may
appear more than once. Therefore the hook stream is neither an exactly-once audit
log nor proof that another client received a message.

Calling arbitrary native operations synchronously could recurse into the same hooks with partially
processed native state. Notices, renames, channel/user/admin operations and fetch starts queue explicit effects to a
safe main-loop point, re-resolve actors/targets, and suppress JS feedback while the
effect runs. This avoids unbounded recursion but also means queuing an effect is
not a success receipt or transaction. Later native rejection does not retract an
already accepted effect; disconnected users may cause it to be skipped.

Reload follows the same preference for explicit transitions: construct an entire
candidate set first, then replace the active set. This protects the previous state
from partial initialization failure, but is not an atomic snapshot of the filesystem.
Success resets JavaScript state. The V8 platform remains process-lifetime and
NODELETE retains necessary module code; replacing native binaries needs a restart.

[API reference](../reference/api.md) specifies authority/effect limits;
[hook reference](../reference/hooks.md) lists patches, native result semantics and exceptions;
[event-provider reference](../reference/events.md) lists the separately subscribed event families.
