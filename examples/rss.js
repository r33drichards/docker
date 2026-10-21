/* SPDX-License-Identifier: MIT */
// Opt-in trusted administrator script. The embedding supplies HTTP and IRC I/O;
// SQLite stores subscriptions and seen IDs. Scheduling/jobs/output queues are transient.
const rssStorage = rssSqliteStore({channel: value => /^#[^ ,:\x00-\x1f]+$/.test(value),
  channelKey: rssChannelKey, feed: value => rssHttpUrl(value, true)});
const rssSubscriptions = rssStorage.load();
let rssStorageReady = false;
function rssInitializeStorage() {
  if (!rssStorageReady) { rssStorage.initialize(); rssStorageReady = true; }
}
const RSS_INTERVAL = 300;
const RSS_MAX_SUBSCRIPTIONS = 32;
const RSS_MAX_ITEMS = 64;
const RSS_MAX_SEEN = 2048;
const RSS_JOB_TIMEOUT = 180;

function rssChannelKey(channel) {
  return channel.toLowerCase().replace(/[\[\]\\^]/g, c => ({'[': '{', ']': '}', '\\': '|', '^': '~'})[c]);
}
function rssClean(value) {
  return String(value || '').replace(/[\u0000-\u001f\u007f]/g, ' ').replace(/\s+/g, ' ').trim();
}
function rssWireText(value, limit = 350) {
  // No TextEncoder in the embedded runtime. Iterate code points, never split UTF-8.
  let result = '', bytes = 0;
  for (const c of rssClean(value)) {
    const code = c.codePointAt(0);
    if (code >= 0xd800 && code <= 0xdfff) continue;
    const size = code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
    if (bytes + size > limit) break;
    bytes += size;
    result += c;
  }
  return result;
}
function rssHttpUrl(value, feed = false) {
  if (typeof value !== 'string' || value.length > 2048 || /[\u0000-\u0020\u007f\\]/.test(value)) return false;
  const parts = /^(https?):\/\/(\[[0-9a-f:.]+\]|[^:\s/@?#]+)(?::([0-9]{1,5}))?([/?#][^\s]*)?$/i.exec(value);
  if (!parts || (parts[3] && (+parts[3] < 1 || +parts[3] > 65535))) return false;
  if (!feed) return true;
  // Fetch policy is narrower than display-only article links.
  return !value.includes('#') && (!parts[3] || +parts[3] === (parts[1].toLowerCase() === 'https' ? 443 : 80));
}
// Deliberately bounded XML subset: no DTD, declared entities, external resources,
// recovery mode or HTML parser. Token boundaries and nesting are scanned explicitly.
function rssParseXml(input) {
  const bad = () => { throw new Error('Invalid or excessive feed XML'); };
  const xmlChar = cp => cp === 9 || cp === 10 || cp === 13 ||
    (cp >= 32 && cp <= 0xd7ff) || (cp >= 0xe000 && cp <= 0xfffd) ||
    (cp >= 0x10000 && cp <= 0x10ffff);
  if (typeof input !== 'string' || input.length > 65536) bad();
  let bytes = 0;
  for (const char of input) {
    const cp = char.codePointAt(0);
    if (!xmlChar(cp)) bad();
    bytes += cp < 128 ? 1 : cp < 2048 ? 2 : cp < 65536 ? 3 : 4;
    if (bytes > 65536) bad();
  }
  const decode = text => {
    let result = '', position = 0;
    while (position < text.length) {
      const amp = text.indexOf('&', position);
      if (amp < 0) return result + text.slice(position);
      result += text.slice(position, amp);
      const end = text.indexOf(';', amp + 1);
      if (end < 0 || end - amp > 12) bad();
      const entity = text.slice(amp + 1, end);
      const named = {amp: '&', lt: '<', gt: '>', quot: '"', apos: "'"};
      if (Object.prototype.hasOwnProperty.call(named, entity)) result += named[entity];
      else {
        let cp;
        if (/^#x[0-9a-fA-F]+$/.test(entity)) cp = parseInt(entity.slice(2), 16);
        else if (/^#[0-9]+$/.test(entity)) cp = parseInt(entity.slice(1), 10);
        else bad();
        if (!xmlChar(cp)) bad();
        result += String.fromCodePoint(cp);
      }
      position = end + 1;
    }
    return result;
  };
  let pos = input[0] === '\ufeff' ? 1 : 0, root = null, nodes = 0;
  const stack = [];
  const space = () => { const start = pos; while (/[\t\n\r ]/.test(input[pos] || 'x')) ++pos; return pos > start; };
  const name = () => {
    const start = pos;
    if (!/[A-Za-z_]/.test(input[pos] || '')) bad();
    ++pos;
    while (/[A-Za-z0-9_.:-]/.test(input[pos] || '')) ++pos;
    const value = input.slice(start, pos);
    if (value.length > 256 || !/^[A-Za-z_][A-Za-z0-9_.-]*(?::[A-Za-z_][A-Za-z0-9_.-]*)?$/.test(value)) bad();
    return value;
  };
  const text = value => {
    if (stack.length) stack[stack.length - 1].parts.push(value);
    else if (value.trim()) bad();
  };
  while (pos < input.length) {
    if (input[pos] !== '<') {
      let end = input.indexOf('<', pos);
      if (end < 0) end = input.length;
      const raw = input.slice(pos, end);
      if (raw.includes(']]>')) bad();
      text(decode(raw)); pos = end; continue;
    }
    if (input.startsWith('<!--', pos)) {
      const end = input.indexOf('-->', pos + 4);
      if (end < 0 || input.slice(pos + 4, end).includes('--') || input[end - 1] === '-') bad();
      pos = end + 3; continue;
    }
    if (input.startsWith('<![CDATA[', pos)) {
      if (!stack.length) bad();
      const end = input.indexOf(']]>', pos + 9);
      if (end < 0) bad();
      text(input.slice(pos + 9, end)); pos = end + 3; continue;
    }
    if (input.startsWith('<?', pos)) {
      // Processing instructions are inert. XML declarations can occur only first.
      const start = pos;
      pos += 2;
      const target = name();
      if (target.toLowerCase() === 'xml' && (target !== 'xml' || start > (input[0] === '\ufeff' ? 1 : 0))) bad();
      const end = input.indexOf('?>', pos);
      if (end < 0 || (pos !== end && !/[\t\n\r ]/.test(input[pos]))) bad();
      if (target === 'xml' && !/^[\t\n\r ]+version[\t\n\r ]*=[\t\n\r ]*(?:"1\.0"|'1\.0')(?:[\t\n\r ]+encoding[\t\n\r ]*=[\t\n\r ]*(?:"[Uu][Tt][Ff]-8"|'[Uu][Tt][Ff]-8'))?(?:[\t\n\r ]+standalone[\t\n\r ]*=[\t\n\r ]*(?:"(?:yes|no)"|'(?:yes|no)'))?[\t\n\r ]*$/.test(input.slice(pos, end))) bad();
      pos = end + 2; continue;
    }
    if (input.startsWith('<!', pos)) bad();
    if (input.startsWith('</', pos)) {
      pos += 2;
      const closing = name(); space();
      if (input[pos++] !== '>' || !stack.length || stack.pop().name !== closing) bad();
      continue;
    }
    ++pos;
    const tag = name(), attributes = new Map();
    let empty = false;
    while (true) {
      const separated = space();
      if (input.startsWith('/>', pos)) { pos += 2; empty = true; break; }
      if (input[pos] === '>') { ++pos; break; }
      if (!separated || attributes.size >= 32) bad();
      const key = name(); space();
      if (attributes.has(key) || input[pos++] !== '=') bad();
      space();
      const quote = input[pos++];
      if (quote !== '"' && quote !== "'") bad();
      const end = input.indexOf(quote, pos);
      if (end < 0 || end - pos > 4096 || input.slice(pos, end).includes('<')) bad();
      attributes.set(key, decode(input.slice(pos, end))); pos = end + 1;
    }
    if (++nodes > 512 || stack.length >= 32) bad();
    const namespaces = new Map(stack.length ? stack[stack.length - 1].namespaces : [['xml', 'http://www.w3.org/XML/1998/namespace']]);
    for (const [key, value] of attributes) {
      if (key === 'xmlns') {
        if (value === 'http://www.w3.org/XML/1998/namespace' || value === 'http://www.w3.org/2000/xmlns/') bad();
        namespaces.set('', value);
      }
      else if (key.startsWith('xmlns:')) {
        const prefix = key.slice(6);
        if (!value || prefix === 'xmlns' || value === 'http://www.w3.org/2000/xmlns/' ||
          ((prefix === 'xml') !== (value === 'http://www.w3.org/XML/1998/namespace'))) bad();
        namespaces.set(prefix, value);
      }
    }
    if (namespaces.size > 64) bad();
    const resolve = (qualified, attribute = false) => {
      const colon = qualified.indexOf(':');
      if (colon < 0) return [attribute ? '' : (namespaces.get('') || ''), qualified];
      const prefix = qualified.slice(0, colon);
      if (!namespaces.has(prefix)) bad();
      return [namespaces.get(prefix), qualified.slice(colon + 1)];
    };
    const expanded = new Set();
    for (const key of attributes.keys()) {
      if (key === 'xmlns' || key.startsWith('xmlns:')) continue;
      const identity = resolve(key, true).join('\n');
      if (expanded.has(identity)) bad();
      expanded.add(identity);
    }
    const [ns, local] = resolve(tag);
    const node = {name: tag, ns, local, attributes, namespaces, parts: []};
    if (stack.length) stack[stack.length - 1].parts.push(node);
    else { if (root) bad(); root = node; }
    if (!empty) stack.push(node);
  }
  if (!root || stack.length) bad();
  return root;
}
function rssParseFeed(body) {
  try {
    const root = rssParseXml(body), atom = 'http://www.w3.org/2005/Atom';
    const children = (node, local, ns) => node.parts.filter(part => typeof part !== 'string' && part.local === local && part.ns === ns);
    const content = node => {
      if (!node) return '';
      let value = '';
      const work = [...node.parts].reverse();
      while (work.length) {
        const part = work.pop();
        if (typeof part === 'string') { value += part; if (value.length > 2048) return ''; }
        else for (let i = part.parts.length - 1; i >= 0; --i) work.push(part.parts[i]);
      }
      return value.trim();
    };
    let entries, ns;
    if (root.local === 'rss' && root.ns === '') {
      if (root.attributes.has('version') && root.attributes.get('version') !== '2.0') return null;
      const channels = children(root, 'channel', '');
      if (channels.length !== 1) return null;
      ns = ''; entries = children(channels[0], 'item', ns);
    } else if (root.local === 'feed' && root.ns === atom) {
      ns = atom; entries = children(root, 'entry', ns);
    } else return null;
    const items = [];
    for (const entry of entries.slice(0, RSS_MAX_ITEMS)) {
      const first = local => children(entry, local, ns)[0];
      let url;
      if (ns) {
        const link = children(entry, 'link', ns).find(node => !node.attributes.has('rel') || node.attributes.get('rel') === 'alternate');
        url = link ? link.attributes.get('href') : '';
      } else url = content(first('link'));
      if (!rssHttpUrl(url)) continue;
      const id = content(first(ns ? 'id' : 'guid')) || url;
      items.push({id, title: content(first('title')), url});
    }
    return items;
  } catch (_) { return null; }
}

function rssRemember(sub, id) {
  sub.seen.set(id, true);
  while (sub.seen.size > (sub.seenLimit || RSS_MAX_SEEN)) sub.seen.delete(sub.seen.keys().next().value);
}
function rssRetry(sub, now) {
  sub.failures = Math.min(sub.failures + 1, 5);
  sub.nextDue = now + Math.min(RSS_INTERVAL * (2 ** (sub.failures - 1)), 3600);
}

ircd.on('postCommand', event => {
  if (event.command !== 'RSS' || !event.validated || event.result !== 'SUCCESS') return;
  const user = ircd.userById(event.userId);
  if (!user || event.params.length !== 2) return;
  const respond = (ok, message) => { ircd.notice(event.userId, message); return {rssOutcome: ok ? 'success' : 'failure'}; };
  const [channel, feed] = event.params;
  if (typeof channel !== 'string' || !ircd.isChannel(channel)) return;
  try { rssInitializeStorage(); } catch (_) { return respond(false, 'RSS storage unavailable; no change made.'); }
  const channelKey = rssChannelKey(channel);
  if (/^list(?::[1-9][0-9]*)?$/.test(feed)) {
    const feeds = [...rssSubscriptions.values()].filter(sub => sub.channelKey === channelKey);
    if (!feeds.length) return respond(true, 'RSS list: no subscriptions for ' + channel + '.');
    const page = feed === 'list' ? 1 : Number(feed.slice(5));
    const pages = Math.ceil(feeds.length / 10);
    if (!Number.isSafeInteger(page) || page > pages) return respond(false, 'RSS list page out of range. Use /RSS ' + channel + ' list.');
    for (const sub of feeds.slice((page - 1) * 10, page * 10)) ircd.notice(event.userId, 'RSS feed: ' + rssWireText(sub.feed));
    return respond(true, 'RSS list complete: ' + feeds.length + ' subscription(s) for ' + channel + ', page ' + page + '/' + pages + (page < pages ? '. Next: /RSS ' + channel + ' list:' + (page + 1) : '.'));
  }
  if (feed === 'off') {
    for (const [key, sub] of rssSubscriptions) {
      if (sub.channelKey === channelKey) {
        try { rssStorage.remove(sub.channel, sub.feed); } catch (_) {
          return respond(false, 'RSS storage unavailable; some subscriptions may remain.');
        }
        if (sub.job !== null) ircd.cancelFetch(sub.job);
        rssSubscriptions.delete(key);
      }
    }
    return respond(true, 'RSS subscriptions removed for ' + channel + '.');
  }
  if (!rssHttpUrl(feed, true)) {
    return respond(false, 'RSS requires a public HTTP(S) feed URL without credentials or fragments, on its default port.');
  }
  const key = channelKey + '\n' + feed;
  if (rssSubscriptions.has(key)) {
    return respond(true, 'RSS subscription already exists.');
  }
  if (rssSubscriptions.size >= RSS_MAX_SUBSCRIPTIONS) {
    return respond(false, 'RSS subscription limit reached.');
  }
  try { rssStorage.add(channel, feed); } catch (_) { return respond(false, 'RSS storage unavailable; subscription not added.'); }
  rssSubscriptions.set(key, {
    channel, channelKey, feed, seen: new Map(), seeded: false, pending: [],
    job: null, startedAt: 0, nextDue: 0, failures: 0,
  });
  return respond(true, 'RSS subscribed. The first successful fetch establishes a baseline; new entries will be posted.');
});

ircd.on('backgroundTimer', event => {
  const now = event.curtime;
  if (!Number.isFinite(now)) return;
  try { rssInitializeStorage(); } catch (_) { return; }
  let posts = 0, starts = 0, completions = 0;
  const ordered = [...rssSubscriptions.values()].sort((a, b) =>
    (a.job === null) - (b.job === null) || a.startedAt - b.startedAt);
  let activeJobs = ordered.filter(sub => sub.job !== null).length;
  for (const sub of ordered) {
    if (sub.storageRetryAt && now < sub.storageRetryAt) continue;
    if (sub.job !== null) {
      // Consume at most one response per callback: XML parsing uses the V8 time budget.
      const result = completions < 1 ? ircd.takeFetchResult(sub.job) : null;
      if (result !== null) {
        ++completions;
        sub.job = null;
        --activeJobs;
        const items = result.ok && result.status === 200 && typeof result.body === 'string' ? rssParseFeed(result.body) : null;
        if (items === null) {
          rssRetry(sub, now);
        } else {
          const unique = new Map();
          for (const item of items) {
            if (!item || typeof item.id !== 'string' || !item.id || item.id.length > 4096) continue;
            if (!rssHttpUrl(item.url)) continue;
            // Keep the full link; never turn a truncated URL into a different destination.
            if (rssWireText(item.url, 300) !== item.url) continue;
            let urlBytes = 0;
            for (const c of item.url) {
              const cp = c.codePointAt(0);
              urlBytes += cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
            }
            if (!unique.has(item.id)) unique.set(item.id, {
              id: item.id, text: '[RSS] ' + rssWireText(item.title, 343 - urlBytes) + ' ' + item.url,
            });
          }
          if (!sub.seeded) {
            try { sub.seenLimit = rssStorage.remember(sub.channel, sub.feed, [...unique.keys()], true); }
            catch (_) { rssRetry(sub, now); continue; }
            for (const id of unique.keys()) rssRemember(sub, id);
            sub.seeded = true;
          } else {
            // Feeds conventionally list newest first. Deliver new entries oldest first.
            // Indexed lookup of only this bounded batch avoids replaying all saved
            // history at startup and makes the database the dedup source of truth.
            let known;
            try { known = rssStorage.known(sub.channel, sub.feed, [...unique.keys()]); }
            catch (_) { rssRetry(sub, now); continue; }
            sub.pending = [...unique.values()].filter(item => !known.has(item.id)).reverse();
          }
          sub.failures = 0;
          sub.nextDue = now + RSS_INTERVAL;
        }
      } else if (now < sub.startedAt || now - sub.startedAt >= RSS_JOB_TIMEOUT) {
        // Native work is not persisted. A restored job token must never stall a feed.
        ircd.cancelFetch(sub.job);
        sub.job = null;
        --activeJobs;
        rssRetry(sub, now);
      }
    }
    while (sub.pending.length && posts < 8) {
      const item = sub.pending[0];
      // Commit before queuing IRC output: failed storage never emits a post.
      // A crash/veto after commit can lose delivery; this is not exactly-once.
      try { sub.seenLimit = rssStorage.remember(sub.channel, sub.feed, [item.id]); }
      catch (_) { sub.storageRetryAt = now + RSS_INTERVAL; break; }
      sub.pending.shift();
      rssRemember(sub, item.id);
      ircd.privmsg(sub.channel, item.text);
      ++posts;
    }
    if (sub.job === null && sub.pending.length === 0 && now >= sub.nextDue && starts < 4 && activeJobs < 8) {
      ++starts;
      let job = null;
      try { job = ircd.fetch(sub.feed); } catch (_) { /* Invalid URL or full native queue. */ }
      if (job !== null) {
        ++activeJobs;
        sub.job = job;
        sub.startedAt = now;
      } else {
        rssRetry(sub, now);
      }
    }
  }
});

// BEGIN RSS SQLITE ADAPTER
/* SPDX-License-Identifier: MIT */
// Durable RSS storage using the explicit ircd.sqlite API.
// Compose this function into rss.js's SAME source/context, not a separate script.
// Configure the trusted script with databases="rss" and an admin-owned 0700
// sqlitedirectory. Call initialize/mutators only in active hooks.
// Catch storage errors before advancing live state; do not retry inside a hook.
// The native 100k-step/25ms SQL guards do not hard-bound filesystem fsync time.
// SQLite commits are independent of deferred IRC actions: send + markSeen is NOT
// atomic. A crash between the two can duplicate a post (or lose one if reversed).
function rssSqliteStore(validators) {
  const db = ircd.sqlite.open('rss');
  let initialized = false;
  const valid = (channel, feed) => typeof channel === 'string' && channel.length <= 200 &&
    validators.channel(channel) && validators.feed(feed);
  const keyFor = (channel, feed) => validators.channelKey(channel) + '\n' + feed;
  const rows = (sql, params = []) => {
    const result = db.query(sql, params);
    if (!result || !Array.isArray(result.rows)) throw new Error('Invalid SQLite query result');
    return result.rows;
  };
  const version = () => {
    const tables = rows("SELECT name FROM sqlite_master WHERE type = 'table' AND name IN ('rss_meta','rss_subscriptions','rss_seen') ORDER BY name");
    if (!tables.length) return 0;
    if (tables.length !== 3) throw new Error('Incomplete RSS database schema');
    const versions = rows('SELECT version FROM rss_meta WHERE singleton = 1');
    if (versions.length !== 1 || versions[0][0] !== 1) throw new Error('Unsupported RSS database version');
    return 1;
  };
  const requireReady = () => { if (!initialized) throw new Error('Initialize RSS storage in an active callback first'); };
  const checkIds = ids => {
    if (!Array.isArray(ids) || ids.length > 64 || ids.some(id => typeof id !== 'string' || !id || id.length > 4096))
      throw new Error('Invalid RSS seen batch');
  };
  const bytes = value => {
    let length = 0;
    for (const char of value) {
      const cp = char.codePointAt(0);
      length += cp < 128 ? 1 : cp < 2048 ? 2 : cp < 65536 ? 3 : 4;
    }
    return length;
  };
  const insertSeen = (subscription, previousWidth, ids) => {
    // Retain at most 128KiB of ID payload per feed (plus SQLite/index overhead).
    // Long IDs therefore reduce the dedup history below 2048.
    // Bound disk use even with 32 feeds and maximal Unicode IDs. Integer foreign
    // keys avoid repeating a potentially 2KiB feed URL in every row and index.
    const width = Math.max(previousWidth, ...ids.map(bytes), 1);
    const retain = Math.min(2048, Math.floor(131072 / width));
    for (const id of ids.slice(-retain)) db.exec('INSERT OR IGNORE INTO rss_seen (subscription, entry_id) VALUES (?, ?)', [subscription, id]);
    db.exec('UPDATE rss_subscriptions SET max_id_bytes = ? WHERE id = ?', [width, subscription]);
    const cutoff = rows('SELECT seq FROM rss_seen WHERE subscription = ? ORDER BY seq DESC LIMIT 1 OFFSET ?', [subscription, retain]);
    if (cutoff.length) db.exec('DELETE FROM rss_seen WHERE subscription = ? AND seq <= ?', [subscription, cutoff[0][0]]);
    return retain;
  };
  return {
    initialize() {
      db.transaction(() => {
        if (version() === 0) {
          db.exec('CREATE TABLE rss_meta (singleton INTEGER PRIMARY KEY CHECK (singleton = 1), version INTEGER NOT NULL)');
          db.exec('CREATE TABLE rss_subscriptions (id INTEGER PRIMARY KEY, key TEXT NOT NULL UNIQUE, max_id_bytes INTEGER NOT NULL DEFAULT 1, channel TEXT NOT NULL, feed TEXT NOT NULL, seeded INTEGER NOT NULL CHECK (seeded IN (0,1)))');
          db.exec('CREATE TABLE rss_seen (seq INTEGER PRIMARY KEY AUTOINCREMENT, subscription INTEGER NOT NULL, entry_id TEXT NOT NULL, UNIQUE (subscription, entry_id))');
          db.exec('CREATE INDEX rss_seen_subscription_seq ON rss_seen (subscription, seq)');
          db.exec('INSERT INTO rss_meta (singleton, version) VALUES (1, 1)');
        }
      });
      initialized = true;
    },
    // Startup-safe, read-only. No history replay is needed before scheduling.
    load() {
      const subscriptions = new Map();
      if (!version()) return subscriptions;
      const stored = rows('SELECT key, channel, feed, seeded, id FROM rss_subscriptions ORDER BY key LIMIT 33');
      if (stored.length > 32) throw new Error('RSS database exceeds subscription limit');
      for (const [key, channel, feed, seeded, storageId] of stored) {
        if (!((Number.isSafeInteger(storageId) && storageId > 0) || (typeof storageId === 'bigint' && storageId > 0n && storageId <= 9223372036854775807n)) || !valid(channel, feed) || key !== keyFor(channel, feed) || (seeded !== 0 && seeded !== 1)) continue;
        subscriptions.set(key, {storageId, channel, channelKey: validators.channelKey(channel), feed,
          seeded: seeded === 1, seen: new Map(), pending: [], job: null,
          startedAt: 0, nextDue: 0, failures: 0, restoring: false, restoreCursor: 0});
      }
      return subscriptions;
    },
    known(channel, feed, ids) {
      checkIds(ids);
      if (!ids.length) return new Set();
      const placeholders = ids.map(() => '?').join(',');
      return new Set(rows('SELECT entry_id FROM rss_seen WHERE subscription = (SELECT id FROM rss_subscriptions WHERE key = ?) AND entry_id IN (' + placeholders + ')', [keyFor(channel, feed), ...ids]).map(row => row[0]));
    },
    // Diagnostic/test inspection only; live scheduling uses indexed known().
    restoreSeenPage(sub) {
      sub.restoring = true;
      // Newest first ensures even a corrupt oversized table cannot force unbounded work.
      const stored = rows('SELECT seq, entry_id FROM rss_seen WHERE subscription = ? AND (? = 0 OR seq < ?) ORDER BY seq DESC LIMIT 16', [sub.storageId, sub.restoreCursor, sub.restoreCursor]);
      for (const [seq, id] of stored) {
        if (!((Number.isSafeInteger(seq) && seq > 0) || (typeof seq === 'bigint' && seq > 0n && seq <= 9223372036854775807n)) || (sub.restoreCursor && seq >= sub.restoreCursor))
          throw new Error('Invalid RSS seen sequence');
        sub.restoreCursor = seq;
        if (typeof id === 'string' && id && id.length <= 4096) sub.seen.set(id, true);
      }
      // Bound inspected rows, including malformed entry IDs, not merely accepted IDs.
      sub.restoreCount = (sub.restoreCount || 0) + stored.length;
      if (stored.length < 16 || sub.restoreCount >= 2048) {
        sub.seen = new Map([...sub.seen].reverse());
        sub.restoring = false;
      }
      return !sub.restoring;
    },
    add(channel, feed) {
      requireReady();
      if (!valid(channel, feed)) throw new Error('Invalid RSS subscription');
      const key = keyFor(channel, feed);
      db.transaction(() => {
        if (rows('SELECT key FROM rss_subscriptions WHERE key = ?', [key]).length) return;
        if (rows('SELECT COUNT(*) FROM rss_subscriptions')[0][0] >= 32) throw new Error('RSS subscription limit reached');
        db.exec('INSERT INTO rss_subscriptions (key, channel, feed, seeded) VALUES (?, ?, ?, 0)', [key, channel, feed]);
      });
      return key;
    },
    remove(channel, feed) {
      requireReady();
      const key = keyFor(channel, feed);
      db.transaction(() => {
        db.exec('DELETE FROM rss_seen WHERE subscription = (SELECT id FROM rss_subscriptions WHERE key = ?)', [key]);
        db.exec('DELETE FROM rss_subscriptions WHERE key = ?', [key]);
      });
    },
    remember(channel, feed, ids, baseline = false) {
      requireReady(); checkIds(ids);
      const key = keyFor(channel, feed);
      return db.transaction(() => {
        const stored = rows('SELECT id, max_id_bytes FROM rss_subscriptions WHERE key = ?', [key]);
        if (!stored.length) throw new Error('Unknown RSS subscription');
        if (!Number.isSafeInteger(stored[0][1]) || stored[0][1] < 1 || stored[0][1] > 16384) throw new Error('Invalid RSS ID byte bound');
        const retain = insertSeen(stored[0][0], stored[0][1], ids);
        if (baseline) db.exec('UPDATE rss_subscriptions SET seeded = 1 WHERE key = ?', [key]);
        return retain;
      });
    },
    close() { db.close(); initialized = false; },
  };
}
// END RSS SQLITE ADAPTER
