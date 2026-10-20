/* SPDX-License-Identifier: MIT */
replay.configure({authority: 'user', users: [{id: 'self', local: true}, {id: 'other', local: true}], channels: ['#one']});
let attempted = false;
ircd.on('postCommand', () => {
  attempted = true; ircd.notice('self', 'staged'); ircd.notice('#one', 'forbidden');
});
replay.throws(() => replay.event('postCommand', {userId: 'self'}), /authority/);
replay.assert('attempted === true && replay.effects.length === 0');
