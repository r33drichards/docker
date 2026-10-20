# Subscribe a channel to RSS

The optional `examples/rss.js` script subscribes existing channels to public RSS
or Atom feeds. It uses the IRC daemon's `backgroundTimer` event, not a separate
Node process or JavaScript timers. A generic native HTTP worker fetches responses without blocking the IRC main
loop. Bounded RSS/Atom parsing runs in the JavaScript script. The script keeps subscriptions, seen entry IDs,
pending posts, retry counts and next-fetch times in ordinary JavaScript Maps and
objects for transient runtime state. Subscriptions and seen entry IDs are stored
in a host-granted SQLite database. Startup restores subscriptions read-only;
each fetched batch checks at most 64 IDs using the durable indexed table. There
is no full-history replay or startup warmup. Native request
tokens and pending messages are never restored.

## Enable the script

Build the version containing the generic asynchronous HTTP host APIs and load `m_javascript`. Install
`examples/rss.js` as `scripts/rss.js` under the configured config directory, then
configure it with administrator authority:

```xml
<javascriptsettings sqlitedirectory="/private/owned/sqlite-directory">
<javascript file="scripts/rss.js" authority="admin" databases="rss">
```

The script and subscriptions are opt-in. Installing the code alone does not fetch
feeds or post anything. Only trusted administrator-authored scripts should have
admin authority. Provision the private, daemon-owned 0700 database directory
separately; see [SQLite configuration and limits](../reference/sqlite.md). The
script requires this grant and fails loading rather than silently falling back
to volatile storage. Schema writes occur only in active callbacks, never during
startup. Deploying or provisioning storage is a separate operator action.

## Add or remove a feed

As an IRC operator, send the server command `RSS <channel> <feed-url>`.
In WeeChat, use `/quote` so the client sends this custom command to the server:

```text
/quote RSS #news https://example.org/feed.xml
```

Clients that forward unknown commands directly may also accept
`/rss #news https://example.org/feed.xml`. Run it from a buffer on the intended
server. Operator status is required; channel operator (`@`) status alone is not
enough, and the server's operator class must permit the `RSS` command. Use your
existing operator access; do not paste operator credentials into chat.

A `481` permission error means the connection lacks the required IRC-operator
access; merely joining the channel or holding channel `@` does not satisfy it.
An unknown-command error can mean the client did not forward the command (use
`/quote` in WeeChat) or that the server lacks the command/module.

A successful addition returns `RSS subscribed. The first successful fetch
establishes a baseline; new entries will be posted.` This confirms storage of the
subscription, not a successful fetch or delivered item. An existing pair returns
`RSS subscription already exists.`

The native command validates operator privileges and the channel. The script
validates the feed URL and subscribes that channel and feed pair. Repeating the same pair is idempotent;
different feeds can post to the same channel and the same feed can serve several
channels. Channel spelling is compared using RFC1459 case folding. Use existing
channels. Feed URLs must be public HTTP(S) URLs without embedded credentials or fragments,
on the scheme's default port (80 for HTTP, 443 for HTTPS). Article links are only
displayed, not fetched, and may include fragments or other valid ports.
The transport rejects prohibited network destinations and unsafe feed data.

To remove every feed subscription for a channel:

```text
/quote RSS #news off
```

`off` removes **all** feeds for that channel, not just one URL. There is currently
no RSS list/status subcommand or individual-feed removal command. `/RSS #news list`
is not a listing request; it is rejected as an invalid feed URL. Keep the URLs you
subscribe, or ask the server operator to inspect stored subscriptions read-only.
Do not remove/re-add feeds merely to check status: re-adding establishes a fresh
baseline.

Removing a subscription also discards its pending posts. Already queued native
fetches may finish, but their results are no longer posted.

## Timing and delivery behavior

- The first successful fetch establishes a baseline and does not post historic
  entries. Later new entries produce channel PRIVMSG messages containing a title
  and complete link.
- Successful fetches repeat every five minutes, subject to background timer
  cadence and queue availability. Failures back off from five minutes to at most
  one hour. Only one fetch per subscription is active at a time.
- RSS XML parsing accepts at most 64 KiB UTF-8, 512 XML nodes, depth 32,
  32 attributes per element and 64 namespace bindings. DTD/entity declarations,
  malformed XML and unknown entities are rejected. RSS2 and Atom are supported;
  arbitrary XML/HTML is not. Parsing is bounded independently of the generic
  transport response limit.
- Limits are 32 subscriptions, four fetch starts, eight concurrent requests and eight channel messages per
  timer callback, 64 parsed entries per fetch, one completed response parsed per callback, and 2,048 remembered IDs per
  subscription, additionally capped at 128 KiB of ID text. Large IDs shorten the
  retained history. Older remembered IDs are evicted; feeds that recycle very old
  entries can therefore repost them.
- A batch of new entries is emitted oldest-first, assuming the feed lists newest
  first. Pending output drains before the next fetch. This is not a complete
  historical archive: entries outside the feed's bounded fetched window can be
  missed.
- Output strips IRC controls, newlines and CTCP delimiters. Titles are shortened
  at UTF-8 boundaries. Item links exceeding 300 UTF-8 bytes are skipped rather
  than truncated into broken or misleading links. The complete message body is
  at most 350 UTF-8 bytes.
- Native work is not JavaScript state. An in-flight job with no result after 180
  seconds is abandoned and retried with backoff. This exceeds the bounded native
  queue wait (16 native jobs, each limited to eight seconds). Oldest requests are
  processed first; at normal five-second timer cadence eight RSS completions
  drain within 40 seconds, below the native 60-second ready-result TTL. Long daemon
  pauses can still expire results; they retry with backoff. Unknown job tokens after
  restoration return failure and are retried with backoff.
- A successful `ircd.privmsg` call queues output; it is not a delivery receipt. IRC
  sending and persistence commits are not atomic. Seen IDs commit before output is
  queued, so a crash or later callback veto can lose an item after it was marked
  seen. Database failures leave live state unchanged and emit no corresponding
  post. This is not exactly-once delivery.

## Test the scheduling logic

From the repository root:

```sh
node tests/test_rss.cjs
```

These deterministic tests mock the native API. They verify operator/stage guards,
Map state, initial baselines, duplicate prevention, channel isolation, bounded
work, UTF-8 output, backoff, abandoned jobs and unsubscribe behavior. Generic HTTP
transport and full daemon/SQLite restart tests are separate checks; this test
alone does not establish network or persistence integration correctness.

For actual local and linked-server messaging checks against a built installation:

```sh
INSPIRCD_NATIVE=/path/to/install python3 tests/rss_native.py
```

`tests/rss_fetch_native.py` adds opt-in end-to-end fetch, parse, baseline,
channel-post and repeat-fetch checks. With `RSS_PUBLIC_FEED_URL` set, it uses the
normal production transport and requires direct public DNS/network access.
For deterministic offline integration, build a **separate test-only** module
with `JS_ASYNC_HTTP_TEST_LOOPBACK` defined and run it with `RSS_TEST_LOOPBACK=1`.
That build permits only literal HTTP `127.0.0.1` fixture destinations. The harness
maps its one synthetic feed URL to an ephemeral local HTTP server; the production
RSS script is otherwise unchanged. Never define this macro in shipped builds.
Normal `tests/js_async_http.sh` separately checks production private-IP rejection
and an explicitly compiled fixture transport.
A fixture pass establishes local HTTP-to-IRC integration, not live public-network
connectivity. The fixture also restarts the actual daemon using the same database,
verifies persisted deduplication and exercises an exclusive database lock during
unsubscribe. `node tests/test_rss_sqlite.cjs` separately exercises real SQLite
transactions and file-backed fresh-realm restart (requires Node 24). The portable
scheduler/replay suites use an explicit SQL contract mock, not a native database.
