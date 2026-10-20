/* SPDX-License-Identifier: MIT */
replay.configure({users: [{id: 'self', local: true}], channels: ['#one']});
const heap = new Map();
ircd.on('preCommand', () => {
  heap.set('veto', true); ircd.notice('self', 'discarded'); return {action: 'deny'};
});
ircd.on('postCommand', e => {
  heap.set(e.command, true);
  ircd.notice('#one', 'vanished'); replay.removeTarget('#one');
  ircd.notice('self', 'queued-before-throw'); throw new Error('fixture failure');
});
replay.event('preCommand', {userId: 'self'});
replay.assert("heap.get('veto') === true && replay.effects.length === 0");
replay.throws(() => replay.event('postCommand', {userId: 'self', command: 'failure'}), /fixture failure/);
// Native host effects are rolled back; the JS heap is NOT transactionally rolled back.
replay.assert("heap.get('failure') === true && replay.effects.length === 0");
replay.throws(() => replay.event('preCommand', {userId: 'self'}), /disabled/);
