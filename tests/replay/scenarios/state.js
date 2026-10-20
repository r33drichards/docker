/* SPDX-License-Identifier: MIT */
replay.configure({users: [{id: '001AAA', local: true, isOper: true}], channels: ['#one']});
const subscriptions = new Map();
let completions = 0;
ircd.on('postCommand', e => {
  if (e.command === 'SUB') subscriptions.set(e.params[0], {seen: new Map(), job: null});
  if (e.command === 'OFF') {
    const sub = subscriptions.get(e.params[0]);
    if (sub?.job) ircd.cancelFetch(sub.job);
    subscriptions.delete(e.params[0]);
  }
});
ircd.on('backgroundTimer', () => {
  for (const [channel, sub] of subscriptions) {
    if (!sub.job) { sub.job = ircd.fetch('https://example.org/feed'); continue; }
    const result = ircd.takeFetchResult(sub.job);
    if (!result) continue;
    sub.job = null; ++completions;
    if (result.ok && !sub.seen.has(result.body)) {
      sub.seen.set(result.body, true); ircd.privmsg(channel, result.body);
    }
  }
});
replay.event('postCommand', {command: 'SUB', params: ['#one'], userId: '001AAA'});
replay.assert('subscriptions instanceof Map && subscriptions.size === 1');
replay.advance(1);
replay.assert("subscriptions.get('#one').seen instanceof Map && subscriptions.get('#one').job === '1'");
replay.advance(2); replay.assert('replay.pending().length === 1');
replay.complete('1', {body: 'item-A'}); replay.advance(3);
replay.assert("subscriptions.get('#one').seen.has('item-A') && completions === 1");
replay.assert("replay.effects.length === 1 && replay.effects[0].text === 'item-A'");
replay.advance(4); replay.complete('2', {body: 'item-A'}); replay.advance(5);
replay.assert('completions === 2 && replay.effects.length === 1');
replay.advance(6);
replay.event('postCommand', {command: 'OFF', params: ['#one'], userId: '001AAA'});
replay.assert('subscriptions.size === 0 && replay.pending().length === 0');
if (replay.complete('3', {body: 'stale'})) throw new Error('stale completion accepted');
replay.advance(7); replay.assert('replay.effects.length === 1');
replay.throws(() => replay.advance(6), /monotonic/);
replay.throws(() => replay.checkpoint(), /snapshot adapter unavailable/);
