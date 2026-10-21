# Fetch jobs and webhook migration

The host offers a bounded asynchronous job API, not browser `fetch`. Enable it only
for reviewed `authority="admin"` scripts. A request can transmit data; do not put
credentials or unrelated private IRC content in a URL, body, log or channel message.

## Start, consume and cancel

Call `ircd.fetch(url, options)` inside an active callback. The return value is an
opaque string token, not a Promise or response. Start is staged until the callback
batch accepts; native effects and fetch starts retain their ordering. Poll from a
later timer callback. `takeFetchResult` returns `null` while pending and consumes a
ready result. Unknown, cancelled or already-consumed tokens also return `null`;
this is not a token-existence query. Clear your own token after consumption. Do not poll the same consumed
token indefinitely or interpret `null` as a successful empty response.

~~~javascript
let job = null;
ircd.registerCommand('FETCHSTATUS', 0, 0, true, () => {
  if (job === null) job = ircd.fetch('https://example.org/status');
});
ircd.setTimer(2, true, () => {
  if (job === null) return;
  const result = ircd.takeFetchResult(job);
  if (result === null) return;
  job = null;
  // Inspect result.ok, status and error before using data.
  // result.body is null for invalid UTF-8; bodyBytes preserves exact bytes.
  // Do not automatically relay untrusted remote data into IRC commands.
});
~~~

Options currently support `method: 'GET'|'POST'` and string `body`; no JavaScript
header object. The [generated declarations](../reference/callable-api.md) define
result fields. `cancelFetch(token)` is best-effort cancellation/discard, not reversal
of a transmitted POST. A successful callback or queued token is not a delivery receipt.
HTTP failure, cancellation and process termination need explicit application handling.

## Destination and resource boundaries

Public HTTP(S) destinations on standard ports only; URL credentials, proxies and
redirects are disabled. DNS address validation occurs at socket creation. Requests
are bounded to 64 KiB and responses to 2 MiB; there are at most 16 jobs per script
and two workers. Connect timeout is 3 seconds and the overall request timeout is
8 seconds. Workers own their inputs and never enter V8. Poisoning/disabling a script, reload or unload cancels
or drops that generation's jobs; don't reuse tokens across generations. An accepted
self-unload suppresses later queued starts, not traffic already transmitted.

## Host-injected headers

Header injection is configured by the host, not granted to arbitrary JavaScript.
Use repeated tags, with placeholder paths and no literal credential values:

~~~text
<javascript file="scripts/status.js" authority="admin">
<javascriptfetchheader script="scripts/status.js"
  url="https://api.example.org/status?format=json"
  name="Authorization" valuefile="/private/status-authorization">
~~~

`script` resolves to a canonical admin script path, relative to the configuration
location or absolute. `valuefile` uses the existing secure secret loader: provision
it separately using the host's protected-file procedure, never in source control.
The absolute file must be a regular file owned by the daemon UID, mode `0400` or
`0600`, without symlink path components. Values are read verbatim: 1–1024 bytes,
no control bytes and no trailing newline. Protected parent-directory rules, allowed
header names and aggregate byte bounds also apply; see the
[shared secret-loader contract](../reference/api.md#endpoint-specific-authentication-headers).
An unsafe/missing/changed file rejects the candidate configuration; a successful
rehash is required to load a rotated value. Already transmitted data is not recalled.
At most 32 distinct `(canonical script, exact URL)` rules and eight headers per rule
are accepted. Configuration fails for conflicting legacy headers on the same URL.

Matching normalizes scheme/host case and the effective default port. Path and query
are exact, case-sensitive bytes, including query order; an explicit empty `?` is
distinct from no query. There are no wildcard or prefix matches. Fragments, URL
userinfo, backslashes and dot path segments are rejected. Rules require HTTPS and
do not override the ordinary fetch public-address/standard-port destination checks.
Redirects remain disabled, so injected headers cannot follow a redirect elsewhere.
A nonmatch does not receive these headers. Rules also apply to a matching named
webhook from the specified script; legacy endpoint credentials do not automatically
become generic fetch rules. Fetch does not inherit legacy private/loopback destination
exceptions.

The matched endpoint is explicitly trusted: it can reflect or encode received secrets
in its response, which fetch makes visible to the script. Host-only injection avoids
direct header getters/arguments/logging; it does **not** guarantee secret isolation
from that script. Choose endpoints that do not reflect credentials and minimize each
script's destination scope. Legacy webhook responses are discarded. Never log injected
headers, relay secrets to IRC or broaden a rule to an unrelated destination.

## Legacy webhook adapter

`postWebhook(name, json)` and `webhookStats()` remain the compatibility path for
administrator-named endpoints. A queued boolean is not remote acceptance, and
aggregate counters cannot establish delivery of a particular request. The refactor
shares the HTTP dispatcher rather than requiring a second transport implementation.
Like fetch starts, webhook commitments follow the ordered deferred native-effect
queue: an accepted self-unload suppresses a later webhook start, not an earlier
request that may already have transmitted data.
The compatibility adapter retains its separate limits: one worker, 64 jobs, 1-second
connect/2-second total timeout, 64 KiB combined response bound, with response discarded.
Do not confuse these with generic fetch job limits above.
Keep legacy configuration while migrating the script and verify delivery in an
isolated fixture before removing it. Do not enable both old and new sending paths
for the same event, or retry an uncertain POST blindly.

## Application and test boundaries

Fetching RSS is application code, not an `ircd.rssfeed` primitive. RSS subscriptions,
feed parsing/deduplication and SQLite persistence are in the [separate stack](../reference/features.md).
Replay can test request ordering and heap Maps using controlled completions; real
HTTP/header injection and lifetime behavior require [native integration](replay.md).
