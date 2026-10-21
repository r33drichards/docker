/* SPDX-License-Identifier: MIT */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {createRssSqliteMock} = require('./rss_sqlite_mock.js');
const source = fs.readFileSync(path.join(__dirname, '../examples/rss.js'), 'utf8');
function harness(existingDatabase) {
  const handlers = new Map(), notices = [], jobs = [], results = new Map(), cancelled = [];
  let operator = false, present = true, accepting = true;
  const database = createRssSqliteMock(existingDatabase && existingDatabase.state);
  let storageFailure = false;
  const storage = {
    ...database,
    exec(sql, params = []) { if (storageFailure) throw new Error('storage unavailable'); return database.exec(sql, params); },
  };
  const context = vm.createContext({ircd: {
    sqlite: {open() { return storage; }},
    on(name, fn) { handlers.set(name, fn); },
    userById() { return present ? {isOper: operator} : null; },
    isChannel(name) { return /^#[^ ,:\x00-\x1f]+$/.test(name); },
    notice(target, text) { assert.ok(Buffer.byteLength(text) <= 400); notices.push({target, text}); },
    privmsg(target, text) { assert.ok(Buffer.byteLength(text) <= 400); notices.push({target, text}); },
    fetch(url) { if (!accepting) throw new TypeError('native queue full'); const id = String(jobs.length + 1); jobs.push({id, url}); return id; },
    cancelFetch(id) { cancelled.push(id); results.delete(id); return true; },
    takeFetchResult(id) { const result = results.get(id); if (!result) return null; results.delete(id); return result; },
  }});
  vm.runInContext(source, context, {timeout: 100});
  return {notices, jobs, results, context, handlers, cancelled, database,
    failStorage(value) { storageFailure = value; },
    oper(value) { operator = value; }, present(value) { present = value; }, accepting(value) { accepting = value; },
    tick(now) { return handlers.get('backgroundTimer')({curtime: now}); },
    command(channel = '#rss', feed = 'https://example.com/rss', extra = {}) {
      handlers.get('postCommand')({command: 'RSS', validated: true, result: 'SUCCESS', userId: '001AAAAAA', params: [channel, feed], ...extra});
    },
    finish(id, items, ok = true) {
      const xml = value => String(value || '').replace(/[\x00-\x08\x0b\x0c\x0e-\x1f]/g, '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
      const body = '<rss version="2.0"><channel>' + items.filter(Boolean).map(entry =>
        '<item><guid>' + xml(entry.id) + '</guid><title>' + xml(entry.title) + '</title><link>' + xml(entry.url) + '</link></item>').join('') + '</channel></rss>';
      results.set(String(id), {ok, status: ok ? 200 : 0, body, error: ok ? '' : 'failed'});
    },
    posts() { return notices.filter(n => n.target.startsWith('#')); },
  };
}
const item = (id, title = id) => ({id, title, url: 'https://example.com/' + encodeURIComponent(id)});
{
  const h = harness();
  h.present(false); h.command(); h.present(true);
  h.command('#rss', 'https://example.com/rss', {validated: false});
  h.command('#rss', 'https://example.com/rss', {result: 'FAILURE'});
  h.command('#rss', 'file:///etc/passwd'); h.command('#rss', 'https://user:pass@example.com/rss');
  h.tick(1); assert.equal(h.jobs.length, 0);
  h.command(); h.command('#RSS'); h.tick(2); assert.equal(h.jobs.length, 1);
  h.tick(3); assert.equal(h.jobs.length, 1);
  h.finish(1, [item('old')]); h.tick(4); assert.equal(h.posts().length, 0);
  h.tick(304); assert.equal(h.jobs.length, 2);
  h.finish(2, [item('new'), item('new'), item('old')]); h.tick(305);
  assert.equal(h.posts().length, 1); assert.match(h.posts()[0].text, /new/);
  h.tick(605); h.finish(3, [item('new'), item('old')]); h.tick(606);
  assert.equal(h.posts().length, 1);
  h.command('#rss', 'off'); h.tick(1000); assert.equal(h.jobs.length, 3);
}
{
  const h = harness(); h.command('#one'); h.command('#two'); h.tick(1);
  h.finish(1, []); h.finish(2, []); h.tick(2); h.tick(3); h.tick(303);
  h.finish(3, [item('same')]); h.finish(4, [item('same')]); h.tick(304);
  assert.equal(h.posts().length, 1); assert.equal(h.results.size, 1); h.tick(305);
  assert.deepEqual(h.posts().map(n => n.target), ['#one', '#two']);
}
{
  const h = harness(); h.command(); h.tick(1); h.finish(1, []); h.tick(2); h.tick(302);
  const entries = Array.from({length: 20}, (_, i) => item(String(i), '\x01\r\n' + '😀'.repeat(200)));
  h.finish(2, entries); h.tick(303); assert.equal(h.posts().length, 8);
  h.tick(304); assert.equal(h.posts().length, 16); h.tick(305); assert.equal(h.posts().length, 20);
  for (const n of h.posts()) { assert.ok(Buffer.byteLength(n.text) <= 350); assert.doesNotMatch(n.text, /[\x00-\x1f\x7f\ufffd]/); }
}
{
  const h = harness(); h.command(); h.accepting(false); h.tick(1); h.tick(2);
  assert.equal(h.jobs.length, 0); h.accepting(true); h.tick(301); assert.equal(h.jobs.length, 1);
  h.finish(1, [], false); h.tick(302); h.tick(901); assert.equal(h.jobs.length, 1);
  h.tick(902); assert.equal(h.jobs.length, 2);
  // Lost native handles after restart are eventually retried, not serialized as promises.
  h.tick(1082); h.tick(2282); assert.equal(h.jobs.length, 3);
}
{
  const h = harness(); for (let i = 0; i < 40; ++i) h.command('#c' + i);
  assert.equal(vm.runInContext('rssSubscriptions.size', h.context), 32);
  h.tick(1); assert.equal(h.jobs.length, 4); h.tick(2); assert.equal(h.jobs.length, 8);
  assert.equal(vm.runInContext('[...rssSubscriptions.values()].every(s => s.seen instanceof Map)', h.context), true);
}
{
  const h = harness(); h.command(); h.tick(10); h.command('#RSS', 'off');
  assert.deepEqual(h.cancelled, ['1']);
  h.finish(1, [item('late')]); h.tick(11); h.tick(1000);
  assert.equal(h.posts().length, 0); assert.equal(h.jobs.length, 1);
}
{
  const h = harness(); h.command(); h.tick(1); h.finish(1, []); h.tick(2); h.tick(302);
  h.finish(2, [null, {id: ''}, {id: 'bad', url: 'javascript:alert(1)'},
    {id: 'long', title: 'long', url: 'https://example.com/' + 'a'.repeat(400)},
    item('good', '😀'.repeat(1000))]); h.tick(303);
  assert.equal(h.posts().length, 1); assert.ok(h.posts()[0].text.endsWith('https://example.com/good'));
  assert.ok(Buffer.byteLength(h.posts()[0].text) <= 350);
}
{
  const h = harness();
  for (let i = 0; i < 16; ++i) h.command('#queued' + i);
  const readyAt = new Map(); let completed = 0, maximumWait = 0;
  for (let now = 0; now <= 100; now += 5) {
    for (const [id, ready] of readyAt) {
      if (!h.results.has(id)) { maximumWait = Math.max(maximumWait, now - 5 - ready); readyAt.delete(id); }
      else assert.ok(now - ready < 60, 'ready completion must be consumed before native TTL');
    }
    h.tick(now);
    assert.ok(vm.runInContext('[...rssSubscriptions.values()].filter(s => s.job !== null).length', h.context) <= 8);
    while (completed < h.jobs.length) { const id = String(++completed); h.finish(id, []); readyAt.set(id, now); }
  }
  assert.equal(h.jobs.length, 16);
  assert.equal(vm.runInContext('[...rssSubscriptions.values()].every(s => s.seeded && s.failures === 0)', h.context), true);
  assert.ok(maximumWait <= 40);

}
{
  const h = harness();
  for (const feed of ['https://example.com/rss#fragment', 'https://example.com/rss#',
    'https://example.com:8443/rss', 'http://example.com:443/rss']) h.command('#rss', feed);
  h.tick(1); assert.equal(h.jobs.length, 0);
  h.command('#rss', 'https://example.com:443/rss'); h.tick(2);
  h.finish(1, []); h.tick(3); h.tick(303);
  h.finish(2, [{id: 'fragment', title: 'link', url: 'https://example.com:8443/story#section'}]); h.tick(304);
  assert.equal(h.posts().length, 1);
  assert.ok(h.posts()[0].text.endsWith('https://example.com:8443/story#section'));
}
{
  const h = harness();
  const parse = body => vm.runInContext('rssParseFeed(' + JSON.stringify(body) + ')', h.context);
  const rss = itemBody => '<rss version="2.0"><channel><item>' + itemBody + '</item></channel></rss>';
  const good = rss('<guid>x</guid><title>A &amp; B &#x1f600; &#65; &lt;ok&gt; &quot;yes&quot; &apos;!</title><link>https://example.org/a?x=1&amp;y=2#frag</link>');
  assert.equal(parse('<?xml version = "1.0" encoding = "UTF-8"?>' + good).length, 1);
  assert.equal(parse(good)[0].title, 'A & B 😀 A <ok> "yes" \u0027!');
  assert.equal(parse(good)[0].url, 'https://example.org/a?x=1&y=2#frag');
  assert.equal(parse(rss('<title><![CDATA[A <b> & raw]]></title><link>https://example.org/</link>'))[0].title, 'A <b> & raw');
  const atom = '<?xml version="1.0"?><a:feed xmlns:a="http://www.w3.org/2005/Atom"><a:entry><a:id>atom-id</a:id><a:title>Hello <b xmlns="http://www.w3.org/1999/xhtml">world</b></a:title><a:link rel="self" href="https://example.org/feed"/><a:link href="https://example.org/article#part"/></a:entry></a:feed>';
  assert.equal(parse(atom)[0].title, 'Hello world');
  assert.equal(parse(atom)[0].url, 'https://example.org/article#part');
  assert.equal(parse('<feed xmlns="http://www.w3.org/2005/Atom"><entry><title>Fallback</title><link href="https://example.org/a"/></entry></feed>')[0].id, 'https://example.org/a');
  for (const bad of [
    '<!DOCTYPE rss><rss><channel/></rss>',
    '<!DOCTYPE rss [<!ENTITY x SYSTEM "file:///etc/passwd">]><rss><channel/></rss>',
    '<!ENTITY x "boom"><rss><channel/></rss>',
    '<?xml nonsense?><rss><channel/></rss>', '<?xml?><rss><channel/></rss>',
    '<?xml version="1.0" encoding="utf-16"?><rss><channel/></rss>',
    '<rss><channel><!-- bad ---></channel></rss>',
    '<rss xmlns:q="http://www.w3.org/XML/1998/namespace"><channel/></rss>',
    '<rss><channel>&#X41;</channel></rss>',
    '<rss><channel></rss>', '<rss><channel/>', '<rss/><rss/>',
    '<rss><channel a="1"a="2"/></rss>', '<rss><channel a="1" a="2"/></rss>',
    '<rss><channel a="<bad"/></rss>', '<rss><channel>bad &unknown;</channel></rss>',
    '<rss><channel>&#0;</channel></rss>', '<rss><channel>&#xD800;</channel></rss>',
    '<rss><channel>&#x110000;</channel></rss>', '<rss><channel>]]></channel></rss>',
    '<rss><channel><!-- bad -- comment --></channel></rss>',
    '<rss><channel><![CDATA[unterminated</channel></rss>',
    '<rss><channel><x:item/></channel></rss>', '<rss><channel/></rss>tail',
    '<rss><channel/></rss><?xml version="1.0"?>', '<feed><entry/></feed>',
    '<rss><channel>' + '<x>'.repeat(33) + '</x>'.repeat(33) + '</channel></rss>',
    '<rss><channel>' + '<x/>'.repeat(2050) + '</channel></rss>',
    '<rss><channel>' + '😀'.repeat(262144) + '</channel></rss>',
    '<rss><channel>' + '\ud800' + '</channel></rss>',
  ]) assert.equal(parse(bad), null, bad.slice(0, 100));
  assert.equal(parse(rss('<title>Unsafe</title><link>javascript:alert(1)</link>')).length, 0);
  assert.equal(parse('<rss><channel>' + Array.from({length: 80}, (_, i) => '<item><link>https://example.org/' + i + '</link></item>').join('') + '</channel></rss>').length, 64);
  assert.equal(vm.runInContext('rssWireText("\\x01hello\\r\\nworld")', h.context), 'hello world');
  h.command(); h.tick(1);
  h.results.set('1', {ok: true, status: 200, body: '<rss malformed'}); h.tick(2);
  assert.equal(vm.runInContext('[...rssSubscriptions.values()][0].failures', h.context), 1);
  h.tick(302); h.results.set('2', {ok: true, status: 404, body: good}); h.tick(303);
  assert.equal(vm.runInContext('[...rssSubscriptions.values()][0].failures', h.context), 2);
}
{
  const h = harness(); h.command('#one'); h.command('#two'); h.command('#stale'); h.tick(1);
  h.finish(1, []); h.finish(2, []);
  h.tick(181);
  // A consumed completion must not prevent other expired jobs from being cancelled.
  assert.deepEqual(h.cancelled, ['2', '3']);
  assert.equal(h.results.size, 0);
}
{
  const h = harness();
  const parse = body => { h.context.body = body; return vm.runInContext('rssParseFeed(body)', h.context); };
  const wrap = text => '<rss><channel>' + text + '</channel></rss>';
  assert.ok(parse(wrap('x'.repeat(1048576 - 30))));
  assert.equal(parse(wrap('x'.repeat(1048577 - 30))), null);
  assert.ok(parse(wrap('<x/>'.repeat(510))));
  assert.equal(parse(wrap('<x/>'.repeat(511))), null);
  assert.ok(parse(wrap('&amp;'.repeat(13100))));
}
{
  const h = harness(); h.failStorage(true); h.command(); h.tick(1);
  assert.equal(h.jobs.length, 0);
  assert.equal(vm.runInContext('rssSubscriptions.size', h.context), 0);
  h.failStorage(false); h.command(); h.tick(2); h.finish(1, []); h.tick(3);
  h.tick(303); h.finish(2, [item('durable')]); h.failStorage(true); h.tick(304);
  assert.equal(h.posts().length, 0);
  assert.equal(vm.runInContext('[...rssSubscriptions.values()][0].seen.size', h.context), 0);
  assert.equal(vm.runInContext('[...rssSubscriptions.values()][0].pending.length', h.context), 1);
  h.command('#rss', 'off');
  assert.equal(vm.runInContext('rssSubscriptions.size', h.context), 1);
  h.failStorage(false); h.tick(603); assert.equal(h.posts().length, 0);
  h.tick(604); assert.equal(h.posts().length, 1);
  // A fresh isolate reconstructs persistent subscriptions and dedup IDs, not jobs.
  const restarted = harness(h.database);
  assert.equal(vm.runInContext('rssSubscriptions.size', restarted.context), 1);
  restarted.tick(1); assert.equal(restarted.jobs.length, 1); // indexed lookup needs no history replay
  restarted.tick(2); restarted.finish(1, [item('durable'), item('after-restart')]); restarted.tick(3);
  assert.equal(restarted.posts().length, 1);
  assert.match(restarted.posts()[0].text, /after-restart/);
}
{
  const h = harness(); h.command(); h.tick(1); h.finish(1, [item('baseline')]);
  h.failStorage(true); h.tick(2);
  assert.equal(vm.runInContext('[...rssSubscriptions.values()][0].seeded', h.context), false);
  assert.equal(h.posts().length, 0);
  h.failStorage(false); h.tick(302); h.finish(2, [item('baseline')]); h.tick(303);
  assert.equal(vm.runInContext('[...rssSubscriptions.values()][0].seeded', h.context), true);
  assert.equal(h.posts().length, 0);
}
{
  const initial = harness();
  for (let i = 0; i < 32; ++i) initial.command('#full' + i);
  const db = initial.database.state;
  for (const sub of db.subscriptions) {
    sub.seeded = 1;
    for (let i = 0; i < 2048; ++i) db.seen.push({subscription: sub.id, seq: db.nextSeq++, id: 'saved' + i});
  }
  const restarted = harness(initial.database);
  restarted.tick(0);
  assert.equal(restarted.jobs.length, 4, 'full-capacity startup starts work on first tick');
  assert.equal(vm.runInContext('[...rssSubscriptions.values()].every(s => !s.restoring && s.seen.size === 0)', restarted.context), true);
  restarted.finish(1, [item('saved2047'), item('genuinely-new')]); restarted.tick(5);
  assert.equal(restarted.posts().length, 1);
  assert.match(restarted.posts()[0].text, /genuinely-new/);
}
console.log('PASS: RSS registered-user/stage guards, Map state, deduplication, baseline, multi-channel isolation, bounded scheduling/output, UTF-8 sanitation, backoff, stale jobs and removal');

{
  const h = harness(); h.command(); h.tick(1);
  const large = '<rss><channel><description>' + 'x'.repeat(70000) + '</description><item><guid>large</guid><title>Large feed</title><link>https://example.com/large</link></item></channel></rss>';
  h.results.set('1', {ok:true,status:200,body:large,bodyBytes:new Uint8Array(Buffer.from(large))});
  assert.deepEqual([...h.tick(2).rssFetchOutcomes], ['baseline-ready']);
  assert.equal(h.posts().length, 0);
  h.command('#rss', 'list'); assert.match(h.notices.at(-2).text, /fetch: baseline-ready/);
  h.tick(302);
  const huge = '<rss><channel>' + 'x'.repeat(1048577) + '</channel></rss>';
  h.results.set('2', {ok:true,status:200,body:huge,bodyBytes:new Uint8Array(Buffer.from(huge))});
  assert.deepEqual([...h.tick(303).rssFetchOutcomes], ['oversize']);
  h.command('#rss', 'list'); assert.match(h.notices.at(-2).text, /fetch: oversize/);
  h.tick(603); h.results.set('3',{ok:false,status:0,body:'',error:'private-transport-secret'});
  assert.deepEqual([...h.tick(604).rssFetchOutcomes], ['transport']);
  assert.equal(h.posts().length, 0);
}
console.log('PASS: >64KiB baseline, 1MiB parser cap, retrieval status and sanitized categories');

for (const code of ['dns', 'connect', 'tls', 'timeout', 'oversize', 'cancelled', 'expired', 'raw-secret']) {
 const h = harness(); h.command(); h.tick(1);
 h.results.set('1', {ok:false,status:0,body:'',error:'private-error-secret',errorCode:code});
 const expected = code === 'raw-secret' ? 'transport' : code;
 assert.deepEqual([...h.tick(2).rssFetchOutcomes], [expected]);
 h.command('#rss','list'); assert.match(h.notices.at(-2).text, new RegExp('fetch: '+expected));
 assert.ok(!h.notices.some(n => n.text.includes('secret')));
}
console.log('PASS: fixed native error categories, compatible fallback and redaction');
