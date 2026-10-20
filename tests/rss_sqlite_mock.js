/* SPDX-License-Identifier: MIT */
// Exact RSS adapter SQL fixture, NOT a SQLite implementation or production fallback.
// Real SQL is tested independently in test_rss_sqlite.cjs and native integration.
function createRssSqliteMock(existing) {
  const state = existing || {tables: [], version: 0, subscriptions: [], seen: [], nextId: 1, nextSeq: 1};
  let inTransaction = false;
  const result = rows => ({columns: [], rows, changes: 0n, lastInsertRowid: 0n});
  const subscription = key => state.subscriptions.find(row => row.key === key);
  const handle = {
    state,
    query(sql, p = []) {
      let rows;
      if (sql.startsWith('SELECT name FROM sqlite_master ')) rows = state.tables.map(name => [name]);
      else if (sql === 'SELECT version FROM rss_meta WHERE singleton = 1') rows = state.version ? [[state.version]] : [];
      else if (sql === 'SELECT key, channel, feed, seeded, id FROM rss_subscriptions ORDER BY key LIMIT 33')
        rows = [...state.subscriptions].sort((a,b) => a.key.localeCompare(b.key)).slice(0,33).map(r => [r.key,r.channel,r.feed,r.seeded,r.id]);
      else if (sql === 'SELECT key FROM rss_subscriptions WHERE key = ?') rows = subscription(p[0]) ? [[p[0]]] : [];
      else if (sql === 'SELECT COUNT(*) FROM rss_subscriptions') rows = [[state.subscriptions.length]];
      else if (sql === 'SELECT id, max_id_bytes FROM rss_subscriptions WHERE key = ?') {
        const row = subscription(p[0]); rows = row ? [[row.id,row.width]] : [];
      } else if (sql === 'SELECT seq, entry_id FROM rss_seen WHERE subscription = ? AND (? = 0 OR seq < ?) ORDER BY seq DESC LIMIT 16')
        rows = state.seen.filter(r => r.subscription === p[0] && (!p[1] || r.seq < p[2])).sort((a,b) => b.seq-a.seq).slice(0,16).map(r => [r.seq,r.id]);
      else if (sql.startsWith('SELECT entry_id FROM rss_seen WHERE subscription = (SELECT id FROM rss_subscriptions WHERE key = ?) AND entry_id IN ('))
        rows = state.seen.filter(r => r.subscription === (subscription(p[0]) || {}).id && p.slice(1).includes(r.id)).map(r => [r.id]);
      else if (sql === 'SELECT seq FROM rss_seen WHERE subscription = ? ORDER BY seq DESC LIMIT 1 OFFSET ?')
        rows = state.seen.filter(r => r.subscription === p[0]).sort((a,b) => b.seq-a.seq).slice(p[1],p[1]+1).map(r => [r.seq]);
      else throw new Error('Unsupported RSS mock query: ' + sql);
      return result(rows);
    },
    exec(sql, p = []) {
      if (sql.startsWith('CREATE TABLE ')) {
        const name = sql.split(' ')[2];
        if (state.tables.includes(name)) throw new Error('table exists');
        state.tables.push(name);
      } else if (sql.startsWith('CREATE INDEX ')) { /* index has no observable fixture state */ }
      else if (sql === 'INSERT INTO rss_meta (singleton, version) VALUES (1, 1)') state.version = 1;
      else if (sql === 'INSERT INTO rss_subscriptions (key, channel, feed, seeded) VALUES (?, ?, ?, 0)') {
        if (subscription(p[0])) throw new Error('duplicate key');
        state.subscriptions.push({id: state.nextId++, key:p[0], channel:p[1], feed:p[2], seeded:0, width:1});
      } else if (sql === 'INSERT OR IGNORE INTO rss_seen (subscription, entry_id) VALUES (?, ?)') {
        if (!state.seen.some(r => r.subscription === p[0] && r.id === p[1])) state.seen.push({seq:state.nextSeq++, subscription:p[0], id:p[1]});
      } else if (sql === 'UPDATE rss_subscriptions SET max_id_bytes = ? WHERE id = ?') state.subscriptions.find(r => r.id === p[1]).width = p[0];
      else if (sql === 'UPDATE rss_subscriptions SET seeded = 1 WHERE key = ?') subscription(p[0]).seeded = 1;
      else if (sql === 'DELETE FROM rss_seen WHERE subscription = ? AND seq <= ?') state.seen = state.seen.filter(r => !(r.subscription === p[0] && r.seq <= p[1]));
      else if (sql === 'DELETE FROM rss_seen WHERE subscription = (SELECT id FROM rss_subscriptions WHERE key = ?)') {
        const row = subscription(p[0]); if (row) state.seen = state.seen.filter(r => r.subscription !== row.id);
      } else if (sql === 'DELETE FROM rss_subscriptions WHERE key = ?') state.subscriptions = state.subscriptions.filter(r => r.key !== p[0]);
      else throw new Error('Unsupported RSS mock exec: ' + sql);
      return result([]);
    },
    transaction(fn) {
      if (inTransaction) throw new Error('nested transaction');
      const saved = {...state, tables: [...state.tables], subscriptions: state.subscriptions.map(row => ({...row})), seen: state.seen.map(row => ({...row}))}; inTransaction = true;
      try {
        const value = fn();
        if (value && typeof value.then === 'function') throw new Error('async transaction');
        return value;
      } catch (error) { Object.assign(state, saved); throw error; }
      finally { inTransaction = false; }
    },
    close() {},
  };
  return handle;
}
if (typeof module !== 'undefined') module.exports = {createRssSqliteMock};
