/* SPDX-License-Identifier: MIT */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../examples/javascript/join-webhook.js'), 'utf8');
const callbacks = new Map();
const posts = [];
let accepted = true;
vm.runInNewContext(source, {ircd: {
  on(stage, callback) { assert.equal(callbacks.has(stage), false); callbacks.set(stage, callback); },
  postWebhook(name, body) { posts.push({name, body}); return accepted; },
}}, {timeout: 100});
assert.deepEqual([...callbacks.keys()], ['postJoin']);
const hook = callbacks.get('postJoin');
const text = value => ({text: value, bytes: [], byteLength: value?.length || 0, truncated: false});
const event = () => ({stage: 'postJoin', userId: '001AAAAAA', memb: {
  user: {id: '001AAAAAA', nick: text('alice'), local: true, quitting: false, address: 'PRIVATE-IP'},
  channel: {name: text('#example')}, prefixes: text(''),
}});
const join = event();
assert.equal(hook(join), undefined);
assert.equal(posts.length, 1);
assert.deepEqual(JSON.parse(posts[0].body), {
  event: 'irc.channel.join', user: {id: '001AAAAAA', nick: 'alice'}, channel: '#example',
});
assert.equal(posts[0].name, 'joins');
assert.equal(posts[0].body.includes('PRIVATE-IP'), false);
join.memb.user.nick.text = 'mutated';
assert.equal(JSON.parse(posts[0].body).user.nick, 'alice');
for (const change of [
  e => { e.memb = null; }, e => { e.memb.user = null; },
  e => { e.memb.user.local = false; }, e => { e.memb.user.quitting = true; },
  e => { e.memb.user.nick.text = null; }, e => { e.memb.user.nick.truncated = true; },
  e => { e.memb.channel = null; }, e => { e.memb.channel.name.text = null; },
  e => { e.memb.channel.name.truncated = true; }, e => { e.memb.channel.name = text('#private'); },
  e => { e.memb.user.id = null; },
]) { const e = event(); change(e); assert.equal(hook(e), undefined); }
assert.equal(posts.length, 1);
accepted = false;
assert.equal(hook(event()), undefined);
assert.equal(posts.length, 2); // Queue-full has no local retry.
accepted = true;
hook(event());
assert.equal(posts.length, 3); // A genuine later rejoin is a new event.
console.log('PASS: postJoin-only registration, owned JSON, channel/local/privacy guards, no retry/Promise');
