/* SPDX-License-Identifier: MIT */
// Opt-in only: load exactly once with authority="admin". See docs/WCHAT_NOCODB.md.
// Configure destination name wchat_events separately. No secrets belong here.
(() => {
  'use strict';
  const CHANNEL = '#wchat';
  const DESTINATION = 'wchat_events';
  // Unique within this script generation; random generation nonce separates reloads.
  // This is a record key, not a credential or a durable exactly-once guarantee.
  const generation = Date.now().toString(36) + '-' + Array.from({length: 4},
    () => Math.floor(Math.random() * 0x100000000).toString(16).padStart(8, '0')).join('');
  let sequence = 0n;
  const text = value => value && value.truncated === false &&
    typeof value.text === 'string' ? value.text : null;

  function identity(user) {
    if (!user || user.local !== true || user.quitting ||
        typeof user.id !== 'string' || !user.id) return null;
    const nick = text(user.nick);
    return nick === null ? null : {id: user.id, nick};
  }
  function selected(channel) {
    return !!channel && text(channel.name) === CHANNEL;
  }
  function emit(payload) {
    const key = 'wchat-' + generation + '-' + (++sequence).toString();
    const body = JSON.stringify({key, data: {...payload, observedAt: new Date().toISOString()}});
    // No TextEncoder dependency; count the actual JSON UTF-8 bytes.
    let bytes = 0;
    for (const c of body) {
      const n = c.codePointAt(0);
      bytes += n < 0x80 ? 1 : n < 0x800 ? 2 : n < 0x10000 ? 3 : 4;
      if (bytes > 16384) return; // Never disable the script for oversized text.
    }
    ircd.postWebhook(DESTINATION, body); // Queue-full drops; never retry.
  }
  function membership(event, name) {
    const memb = event && event.memb;
    if (!memb || !selected(memb.channel)) return;
    const user = identity(memb.user);
    if (user) emit({version: 1, event: name, user, channel: CHANNEL});
  }
  ircd.on('postJoin', event => { membership(event, 'irc.channel.join'); });
  ircd.on('userPart', event => { membership(event, 'irc.channel.part'); });
  ircd.on('userPostMessage', event => {
    if (!event || !event.target || !selected(event.target.channel)) return;
    // Do not export status-restricted messages such as @#wchat.
    if (text(event.target.status) !== '' || text(event.target.originalStatus) !== '') return;
    const user = identity(event.user);
    const details = event.details;
    if (!user || !details) return;
    // InspIRCd 4.12.1: PRIVMSG = 0; NOTICE = 1.
    if (details.type !== 0) return; // Ordinary PRIVMSG only, never NOTICE.
    const message = text(details.text);
    if (message === null) return;
    emit({version: 1, event: 'irc.channel.message', user, channel: CHANNEL,
      messageType: 'PRIVMSG', text: message});
  });
})();
