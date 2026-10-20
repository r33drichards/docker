/* SPDX-License-Identifier: MIT */
replay.configure({users: [{id: 'oper', local: true, isOper: true}, {id: 'user', local: true}], channels: ['#one']});
const counts = new Map([['repeat', 0], ['once', 0]]);
const repeat = ircd.setTimer(5, true, () => counts.set('repeat', counts.get('repeat') + 1));
ircd.setTimer(3, false, () => counts.set('once', counts.get('once') + 1));
ircd.registerCommand('TEST', 1, 1, true, e => { counts.set(e.params[0], 42); });
replay.advance(2); replay.assert("counts.get('once') === 0");
replay.advance(5); replay.assert("counts.get('once') === 1 && counts.get('repeat') === 1");
replay.advance(100); replay.assert("counts.get('repeat') === 2"); // no catch-up storm
ircd.clearTimer(repeat); replay.advance(200); replay.assert("counts.get('repeat') === 2");
if (replay.command('TEST', 'user', ['denied'])) throw new Error('oper check bypassed');
replay.assert("!counts.has('denied')");
replay.command('TEST', 'oper', ['allowed']); replay.assert("counts.get('allowed') === 42");
replay.throws(() => ircd.registerCommand('LATE', 0, 0, false, () => {}), /registration/);
