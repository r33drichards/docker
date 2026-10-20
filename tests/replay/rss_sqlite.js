/* SPDX-License-Identifier: MIT */
// Requires explicit rss_sqlite_mock.js preload + real examples/rss.js.
// Tests application ordering under modeled storage failure, NOT SQLite itself.
replay.configure({users: [{id: 'oper', local: true, isOper: true}], channels: ['#feed']});
const sqliteFixture = ircd.sqlite.open('rss');
const sqliteExec = sqliteFixture.exec;
const sqliteKey = '#feed\nhttps://example.org/feed';
const sqliteFeed = ids => '<rss version="2.0"><channel>' + ids.map(id =>
  '<item><guid>' + id + '</guid><title>' + id + '</title><link>https://example.org/' + id + '</link></item>'
).join('') + '</channel></rss>';
const sqliteCommand = feed => replay.event('postCommand', {
  command: 'RSS', validated: true, result: 'SUCCESS', userId: 'oper', params: ['#feed', feed],
});
// Mutation failure must not create a live subscription or leave half a transaction.
sqliteFixture.exec = (sql, params) => {
  if (sql.startsWith('INSERT INTO rss_subscriptions')) throw new Error('injected write failure');
  return sqliteExec(sql, params);
};
sqliteCommand('https://example.org/feed');
replay.assert('rssSubscriptions.size === 0 && sqliteFixture.state.subscriptions.length === 0');
sqliteFixture.exec = sqliteExec;
sqliteCommand('https://example.org/feed');
replay.advance(1); replay.complete('1', {body: sqliteFeed(['old'])}); replay.advance(2);
replay.assert("rssSubscriptions.get(sqliteKey).seeded && sqliteFixture.state.subscriptions[0].seeded === 1");
replay.assert("sqliteFixture.state.seen.length === 1 && sqliteFixture.state.seen[0].id === 'old'");
// Fail AFTER insertion, proving transaction restoration rather than a pre-call failure.
sqliteFixture.exec = (sql, params) => {
  if (sql.startsWith('UPDATE rss_subscriptions SET max_id_bytes')) throw new Error('injected mid-transaction failure');
  return sqliteExec(sql, params);
};
replay.advance(302); replay.complete('2', {body: sqliteFeed(['new', 'old'])}); replay.advance(303);
replay.assert("rssSubscriptions.get(sqliteKey).pending.length === 1 && !rssSubscriptions.get(sqliteKey).seen.has('new')");
replay.assert("sqliteFixture.state.seen.length === 1 && sqliteFixture.state.seen[0].id === 'old'");
replay.assert("replay.effects.every(effect => effect.kind !== 'privmsg')");
replay.assert('rssSubscriptions.get(sqliteKey).storageRetryAt === 603');
sqliteFixture.exec = sqliteExec;
replay.advance(602); replay.assert("replay.effects.every(effect => effect.kind !== 'privmsg')");
replay.advance(603);
replay.assert("rssSubscriptions.get(sqliteKey).seen.has('new') && sqliteFixture.state.seen.length === 2");
replay.assert("replay.effects.filter(effect => effect.kind === 'privmsg').length === 1");
// Load into a new heap Map from the stored records; no in-flight native job survives.
const sqliteRestored = rssStorage.load();
replay.assert('sqliteRestored instanceof Map && sqliteRestored !== rssSubscriptions');
replay.assert('sqliteRestored.get(sqliteKey).job === null && sqliteRestored.get(sqliteKey).restoring');
rssStorage.restoreSeenPage(sqliteRestored.get(sqliteKey));
replay.assert("sqliteRestored.get(sqliteKey).seen instanceof Map && sqliteRestored.get(sqliteKey).seen.has('new') && !sqliteRestored.get(sqliteKey).restoring");
// Failed unsubscribe leaves both heap and stored rows intact.
sqliteFixture.exec = (sql, params) => {
  if (sql === 'DELETE FROM rss_subscriptions WHERE key = ?') throw new Error('injected delete failure');
  return sqliteExec(sql, params);
};
sqliteCommand('off');
replay.assert('rssSubscriptions.size === 1 && sqliteFixture.state.subscriptions.length === 1 && sqliteFixture.state.seen.length === 2');
sqliteFixture.exec = sqliteExec; sqliteCommand('off');
replay.assert('rssSubscriptions.size === 0 && sqliteFixture.state.subscriptions.length === 0 && sqliteFixture.state.seen.length === 0');
