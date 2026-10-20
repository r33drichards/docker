# Opt-in #wchat event forwarding

This adapter sends local-user JOIN, PART and ordinary channel PRIVMSG events to
the configured NocoDB `data` table. It is disabled by default. Enabling it exports
sender UUID, nickname, channel and complete message text to that private service.
Only enable it with authorization for that channel and destination.

## Activation

The primary image packages `examples/javascript/wchat-nocodb.js` at
`/inspircd/scripts/wchat-nocodb.js`. Set `INSPIRCD_NOCODB_WCHAT=yes` to activate
the managed configuration emitter. The token materializer must first prepare
`/inspircd/.secrets/nocodb-token` through the separately authorized secret setup.
The token must not be put in JavaScript, the image, version control or logs.
It is used as an `xc-token` header, not a URL parameter.

The shipped configuration includes `sh conf/nocodb.sh`; existing persisted custom
configuration must explicitly include that emitter once. The emitter is silent
when the activation flag is absent or `no`. Its enabled configuration is equivalent
to the following (do not also add this manually when using the emitter):

```xml
<module name="javascript">
<javascriptwebhook name="wchat_events" url="https://nocodb-render-mvp.onrender.com/api/v2/tables/mdz86ygcx26cnd6/records">
<javascriptwebhookheader endpoint="wchat_events" name="xc-token" valuefile="/inspircd/.secrets/nocodb-token">
<javascript file="/inspircd/scripts/wchat-nocodb.js" authority="admin">
```

Load exactly once. The endpoint name is fixed in the script; its URL and secret
header are administrator configuration and are not readable or selectable by
JavaScript. An invalid activation value or missing/unsafe token must fail
configuration rather than silently pretending that forwarding is active.

## Row format and scope

Each POST is a single NocoDB row with `key` (text) and `data` (JSON).
`data` contains `version: 1`, `event`, `user: {id, nick}`, `channel: '#wchat'`, and
`observedAt` (UTC timestamp from the observer). Message events additionally contain
`messageType: 'PRIVMSG'` and `text`, copied from the final message snapshot.
The three event names are `irc.channel.join`, `irc.channel.part`, and
`irc.channel.message`.

The key combines generation time, a 128-bit random generation nonce, and a
monotonic per-generation counter. Every observed event gets a new key, including
repeated identical messages and genuine rejoins. This avoids content-based
deduplication; it is not a cryptographic ID or a guaranteed global exactly-once
identifier. The table does not become an idempotency ledger merely by storing it.

Only `postJoin`, `userPart` and `userPostMessage` are registered. The adapter:

- Selects exact canonical channel spelling `#wchat` and local-origin users only.
- Excludes DMs, other channels, NOTICE and status-restricted messages such as
  `@#wchat`, including messages originally status-restricted before rewriting.
- Does not export IP/host, PART reasons, message tags or original pre-rewrite text.
- Preserves ordinary PRIVMSG text, including Unicode, spacing and CTCP ACTION
  framing. It does not attempt to classify or remove sensitive content in text.
- Skips invalid/truncated snapshots and rows exceeding 16 KiB in UTF-8, counting
  the complete `key`/`data` envelope.
- Does not interpret QUIT, KICK or network splits as PART events.

`userPostMessage` runs after core acceptance and attempted channel fan-out. It is
not a recipient-read or network-delivery receipt. Failed/blocked messages do not
use this observer. A sender's echo is not a second message event.

The local-only filter prevents duplicate collection on linked servers but omits
remote-origin events on a single collector. Covering a multi-server network would
require one authorized adapter per home server or an explicitly reviewed different
collection policy. The current deployment must not be advertised as a complete
network audit feed without checking its topology.

## Delivery limits

Delivery is best effort: no retries, persistence or acknowledgements in JavaScript.
Queue-full, oversized snapshots, transport errors, crashes and successful rehash
retirement may drop events. HTTP 2xx is only the receiver's HTTP response, not proof
of a durable row visible to readers. NocoDB's documented default API limit is five
requests per second per user; 429 responses may therefore lose bursts under this
one-POST-per-event adapter. No broad or table-only token scope is assumed; verify
the installed NocoDB edition and actual credential permissions before provisioning.

## Validation

`node tests/test_wchat_nocodb.cjs` exercises exact registration, row envelope,
distinct keys for repeated text, UTF-8 size bounds and privacy/routing guards.
`tests/wchat_nocodb_native.py` integrates into the native daemon suite and uses
only an owned loopback receiver with a synthetic header. It verifies actual
JOIN/PART/rejoin, delivered Unicode messages, native blocked-message exclusion,
DM/other-channel/NOTICE/status restrictions and absence of secret fields in rows.
No test connects to the real NocoDB endpoint. Real activation still requires
checking the expected deployed commit, safe secret setup and authorized end-to-end
row verification.
