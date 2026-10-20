/* SPDX-License-Identifier: MIT */
// Run after the real examples/rss.js, not a duplicated algorithm.
replay.configure({users: [{id: 'oper', local: true, isOper: true}, {id: 'user', local: true}], channels: ['#feed', '#second']});
const replayFeed = ids => '<rss version="2.0"><channel>' + ids.map(id =>
  '<item><guid>' + id + '</guid><title>Title ' + id + '</title><link>https://example.org/' + id + '</link></item>'
).join('') + '</channel></rss>';
const replaySubscribe = (channel = '#feed', feed = 'https://example.org/feed', userId = 'user') =>
  replay.event('postCommand', {command: 'RSS', validated: true, result: 'SUCCESS', userId, params: [channel, feed]});
const replaySub = () => rssSubscriptions.get('#feed\nhttps://example.org/feed');
const replayPosts = () => replay.effects.filter(effect => effect.kind === 'privmsg');
replaySubscribe('#feed', 'https://example.org/feed', 'missing');
replay.assert('rssSubscriptions.size === 0');
replaySubscribe(); replaySubscribe('#FEED');
replay.assert('rssSubscriptions instanceof Map && rssSubscriptions.size === 1');
replay.assert('replaySub().seen instanceof Map && !replaySub().seeded');
replay.advance(1); replay.assert("replaySub().job === '1' && replay.pending().length === 1");
replay.advance(2); replay.assert('replay.pending().length === 1');
replay.complete('1', {body: replayFeed(['old'])}); replay.advance(3);
replay.assert("replaySub().seeded && replaySub().seen.has('old') && replaySub().nextDue === 303");
replay.assert('replayPosts().length === 0');
replay.advance(303); replay.complete('2', {body: replayFeed(['new', 'new', 'old'])}); replay.advance(304);
replay.assert("replaySub().seen.size === 2 && replaySub().seen.has('new')");
replay.assert("replayPosts().length === 1 && replayPosts()[0].text === '[RSS] Title new https://example.org/new'");
replay.advance(604); replay.complete('3', {body: replayFeed(['new', 'old'])}); replay.advance(605);
replay.assert('replayPosts().length === 1');
replay.advance(905); replay.complete('4', {body: '<!DOCTYPE rss><rss/>'}); replay.advance(906);
replay.assert('replaySub().failures === 1 && replaySub().nextDue === 1206 && replaySub().seen.size === 2');
replay.advance(1205); replay.assert('replay.pending().length === 0');
replay.advance(1206); replay.complete('5', {ok: false, status: 503, body: ''}); replay.advance(1207);
replay.assert('replaySub().failures === 2 && replaySub().nextDue === 1807');
replay.advance(1807); replay.assert("replaySub().job === '6'");
replaySubscribe('#FEED', 'off');
replay.assert('rssSubscriptions.size === 0 && replay.pending().length === 0');
if (replay.complete('6', {body: replayFeed(['stale'])})) throw new Error('cancelled completion accepted');
replay.advance(1808); replay.assert('replayPosts().length === 1');

// Complete different channels out of order; each retains its own baseline/seen Map.
replaySubscribe(); replaySubscribe('#second'); replay.advance(2000);
replay.complete('8', {body: replayFeed(['second-old'])}); replay.advance(2001);
replay.assert("rssSubscriptions.get('#second\\nhttps://example.org/feed').seen.has('second-old')");
replay.assert('!replaySub().seeded');
replay.complete('7', {body: replayFeed(['first-old'])}); replay.advance(2002);
replay.assert("replaySub().seen.has('first-old') && !replaySub().seen.has('second-old')");
replay.assert('replayPosts().length === 1');
