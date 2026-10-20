# Feature and branch matrix

This documentation describes its own source tree. A passing test or open pull request
is not a release, merge, deployment, or permission to operate a server.

## Review stacks

| Stack | Scope | What it does not imply |
| --- | --- | --- |
| [Native APIs #9](https://github.com/r33drichards/docker/pull/9) → [webhook/fetch #11](https://github.com/r33drichards/docker/pull/11) → documentation | Callable host APIs, managed jobs/timers, authenticated fetch transport, replay tooling and onboarding | SQLite and RSS are not bundled into this branch |
| [Persistent state #13](https://github.com/r33drichards/docker/pull/13) | Incorporates SQLite #10 plus opt-in persistent configuration/data/log layout | Not included by the API/webhook/docs stack; volume/import/deployment require separate authorization |
| [SQLite #10](https://github.com/r33drichards/docker/pull/10) → [RSS #8](https://github.com/r33drichards/docker/pull/8) | Explicit durable database state and RSS subscriptions/seen IDs | Native JavaScript heap snapshots and exactly-once delivery are not provided |

The repository default branch is `master`. Each stacked PR targets its predecessor,
not a published release. Review PR base/head and the actual checkout before copying an
example. These parallel branches touch bridge declarations, build files and transport integration;
merging one does not validate the other. Resolve those overlaps, regenerate references,
and rerun combined native/restart tests before any deployment. Do not blindly apply
both copies of their shared generic-fetch prerequisite.

## Callable capabilities in this stack

| Area | Methods | Boundary |
| --- | --- | --- |
| Observe events | `on` | Core and provider events expose owned snapshots; observation does not initiate a native operation |
| Basic user actions | `userById`, `notice`, `rename` | User-authority effects are restricted to the triggering local client |
| Queries | `userByNick`, `channelByName`, `channels` | Admin only; copied values, not live native objects |
| Commands | `registerCommand` | Admin initialization; local-only; no replacement of native or other-script commands |
| Timers | `setTimer`, `clearTimer` | Admin; main-loop callbacks; no catch-up bursts or durable handles |
| HTTP | `fetch`, `takeFetchResult`, `cancelFetch` | Admin active callbacks; bounded asynchronous jobs, not browser Fetch/Promises |
| Compatibility webhooks | `postWebhook`, `webhookStats` | Configured destinations and JSON schema; aggregate counters are not delivery receipts |
| Channel changes | `join`, `part`, `kick`, `setTopic`, `setModes` | Admin active callbacks; revalidated deferred operations |
| User administration | `setHost`, `setUsername`, `setRealname`, `disconnect` | Displayed identity changes; no operator identity or credentials are returned |
| Server administration | `addBan`, `removeBan`, `loadModule`, `unloadModule`, `rehash`, `shutdown` | Trusted admin scripts only; potentially destructive |
| Utilities/assets | Generated validators/matchers/time/color/random helpers, `wasm` | Strict conversions and copied values; random helpers are not a cryptographic guarantee |

Use the generated [callable reference](callable-api.md) for exact signatures and types.
Paired channel `privmsg`/`notice` support belongs to the separate RSS stack; do not
assume it is available on the native-API branch alone.

## Remaining limits

No raw native pointers, arbitrary SDK binding, standalone DNS resolver, user-mode
setter, real-host setter, browser Promise/microtask API, or automatic native resource
persistence is promised. Header injection is host-configured, not arbitrary JavaScript
header access. SQLite uses explicit data persistence, not preservation of closures,
Maps, timer handles, sockets or in-flight fetches.

The checked-in tests describe reproducible coverage. CI can skip label-gated heavy
jobs; a green quick check does not mean every container, fuzz or live-network job ran.
