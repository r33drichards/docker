// Load once with authority="admin" and a configured javascriptwebhook named joins.
// No fetch, Promise, URL, credentials, IP addresses, or message bodies in this file.
(() => {
  'use strict';
  // Explicit channel opt-in; use the channel's canonical spelling on this server.
  const channels = new Set(['#example']);
  const text = value => value && !value.truncated && typeof value.text === 'string'
    ? value.text : null;

  ircd.on('postJoin', event => {
    const membership = event.memb;
    const user = membership && membership.user;
    const channel = membership && membership.channel;
    if (!user || user.local !== true || user.quitting || typeof user.id !== 'string') return;
    const nick = text(user.nick);
    const name = channel && text(channel.name);
    if (nick === null || name === null || !channels.has(name)) return;
    // true means staged, not delivered. Queue-full false is deliberately dropped;
    // do not retry an ambiguous HTTP outcome or return a Promise from a hook.
    ircd.postWebhook('joins', JSON.stringify({
      event: 'irc.channel.join', user: {id: user.id, nick}, channel: name,
    }));
  });
})();
