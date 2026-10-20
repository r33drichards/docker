/* SPDX-License-Identifier: MIT */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const hooks = new Map(), posts = [];
let accepts = true;
vm.runInNewContext(fs.readFileSync(__dirname + '/../examples/javascript/wchat-nocodb.js', 'utf8'), {ircd: {
  on(name, fn) { assert(!hooks.has(name)); hooks.set(name, fn); },
  postWebhook(name, body) { posts.push({name, row: JSON.parse(body), bytes: Buffer.byteLength(body)}); return accepts; }
}});
const text = s => ({text:s, truncated:false});
const user = () => ({id:'001AAAAAA', nick:text('alice'), local:true, quitting:false, address:text('PRIVATE')});
const channel = () => ({name:text('#wchat')});
const member = () => ({memb:{user:user(), channel:channel()}, partmessage:text('SECRET REASON')});
const msg = () => ({user:user(), target:{channel:channel(), status:text(''), originalStatus:text('')},
  details:{type:0,text:text('hello λ'),originalText:text('UNEXPORTED'),tagsIn:[{secret:'TOKEN'}]}});
assert.deepEqual([...hooks.keys()], ['postJoin','userPart','userPostMessage']);
hooks.get('postJoin')(member()); hooks.get('userPart')(member()); hooks.get('userPostMessage')(msg());
assert.equal(posts.length,3);
assert.equal(posts[2].name, 'wchat_events');
const observedAt = posts[2].row.data.observedAt;
assert(Number.isFinite(Date.parse(observedAt)));
assert.deepEqual(posts[2].row.data,{version:1,event:'irc.channel.message',
  user:{id:'001AAAAAA',nick:'alice'},channel:'#wchat',messageType:'PRIVMSG',text:'hello λ',observedAt});
for (const change of [e=>e.user.local=false,e=>e.user.quitting=true,e=>e.user.nick.truncated=true,
  e=>e.target.channel=null,e=>e.target.channel.name=text('#other'),e=>e.target.status=text('@'),
  e=>e.target.originalStatus=text('+'),e=>e.details.type=1,e=>e.details.text.truncated=true,
  e=>e.details.text=text('λ'.repeat(10000)),e=>e.details.text=text('x'.repeat(16384))]) {
  const e=msg();change(e);hooks.get('userPostMessage')(e);
}
assert.equal(posts.length,3);
const foreign=member();foreign.memb.user.local=false;hooks.get('postJoin')(foreign);
const privatePart=member();privatePart.memb.channel.name=text('#private');hooks.get('userPart')(privatePart);
assert.equal(posts.length,3);
accepts=false;hooks.get('userPostMessage')(msg());assert.equal(posts.length,4);
accepts=true;hooks.get('userPostMessage')(msg());assert.equal(posts.length,5); // Repeated text is a real new event.
assert(!JSON.stringify(posts).match(/PRIVATE|SECRET|TOKEN|UNEXPORTED/));
assert.equal(new Set(posts.map(p => p.row.key)).size, posts.length);
assert(posts.every(p => p.bytes <= 16384 && Object.keys(p.row).sort().join(',') === 'data,key'));
console.log('PASS: exact hooks/schema; join/part/message; privacy/status/remote/NOTICE/truncation/size filters; no retry/content dedup');
