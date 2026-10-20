/* SPDX-License-Identifier: MIT */
/* Deterministic host model. Runs in the embedded V8 runtime or a Node vm realm.
 * This models host effects; native routing/security remain integration-test duties.
 */
'use strict';
const replay = (() => {
  const handlers = new Map(), commands = new Map(), timers = new Map(), jobs = new Map();
  const users = new Map(), channels = new Set(), effects = [], trace = [];
  let now = 0, sequence = 0, active = null, staged = null, started = false, disabled = false;
  let authority = 'admin';
  const clone = value => JSON.parse(JSON.stringify(value));
  const admin = () => { if (!active || authority !== 'admin') throw new Error('admin authority required'); };
  const wire = text => {
    if (typeof text !== 'string' || /[\x00\r\n]/.test(text) || text.length > 400)
      throw new Error('invalid wire text');
  };
  const invoke = (callback, event) => {
    if (disabled) throw new Error('script disabled until reload');
    if (active) throw new Error('reentrant dispatch');
    active = event; staged = [];
    try {
      const result = callback(event);
      if (result instanceof Promise) throw new Error('asynchronous callback unsupported');
      if (!(result && result.action === 'deny')) {
        for (const effect of staged) {
          // Resolve targets again at flush, never retain native references.
          if (effect.target && !users.has(effect.target) && !channels.has(effect.target)) continue;
          effects.push(clone(effect));
        }
      }
      return result;
    } catch (error) {
      disabled = true;
      throw error;
    } finally { active = null; staged = null; }
  };
  const message = (kind, target, text) => {
    if (!active) throw new Error('callback required');
    wire(text);
    if (authority !== 'admin' && (target !== active.userId || !users.get(target)?.local))
      throw new Error('target authority denied');
    staged.push({kind, target, text});
  };
  const ircd = Object.freeze({
    on(name, callback) {
      if (started) throw new Error('registration after initialization');
      if (typeof callback !== 'function') throw new TypeError('callback required');
      const list = handlers.get(name) || []; list.push(callback); handlers.set(name, list);
    },
    userById(id) { return users.has(id) ? clone(users.get(id)) : null; },
    isChannel(name) { return typeof name === 'string' && /^#[^ ,:\x00-\x1f]+$/.test(name); },
    time() { return now; },
    notice(target, text) { message('notice', target, text); },
    privmsg(target, text) { message('privmsg', target, text); },
    registerCommand(name, min, max, operOnly, callback) {
      if (started || authority !== 'admin' || commands.has(name)) throw new Error('command registration denied');
      commands.set(name, {min, max, operOnly, callback});
    },
    setTimer(seconds, repeat, callback) {
      if (authority !== 'admin' || seconds < 1 || seconds > 86400 || timers.size >= 32)
        throw new Error('timer denied');
      const id = ++sequence; timers.set(id, {seconds, repeat, callback, due: now + seconds}); return id;
    },
    clearTimer(id) { return timers.delete(id); },
    fetch(url, options = {}) {
      admin();
      const id = String(++sequence);
      jobs.set(id, {id, url, options: clone(options), generation: 1, result: null});
      trace.push({kind: 'fetch', id, url, at: now}); return id;
    },
    takeFetchResult(id) {
      admin(); const job = jobs.get(id); if (!job || !job.result) return null;
      jobs.delete(id); return job.result;
    },
    cancelFetch(id) { admin(); return jobs.delete(id); },
  });
  return Object.freeze({
    ircd, effects, trace,
    configure(options = {}) {
      if (started) throw new Error('configuration after initialization');
      authority = options.authority || 'admin';
      for (const user of options.users || []) users.set(user.id, clone(user));
      for (const channel of options.channels || []) channels.add(channel);
    },
    start() { started = true; },
    event(name, value = {}) {
      started = true;
      const event = clone(value); trace.push({kind: 'event', name, at: now});
      for (const callback of handlers.get(name) || []) {
        const result = invoke(callback, event); if (result && result.action === 'deny') return result;
      }
    },
    command(name, userId, params = []) {
      started = true; const command = commands.get(name);
      if (!command) throw new Error('unknown command');
      if (params.length < command.min || params.length > command.max ||
          (command.operOnly && !users.get(userId)?.isOper)) return false;
      invoke(command.callback, {userId, params: clone(params)}); return true;
    },
    advance(time) {
      if (!Number.isFinite(time) || time < now) throw new Error('clock must be monotonic');
      now = time; started = true;
      // Capture IDs only: a callback may cancel another timer before its turn.
      for (const id of [...timers.keys()]) {
        const timer = timers.get(id); if (!timer || timer.due > now) continue;
        if (timer.repeat) timer.due = now + timer.seconds; else timers.delete(id);
        invoke(timer.callback, {userId: null, params: []});
      }
      this.event('backgroundTimer', {curtime: now});
    },
    complete(id, result = {}) {
      const job = jobs.get(String(id)); if (!job) return false;
      if (job.result) throw new Error('duplicate completion');
      const body = result.body === undefined ? '' : result.body;
      job.result = {ok: true, status: 200, body, bodyBytes: new Uint8Array(), error: null, ...result};
      trace.push({kind: 'complete', id: String(id), at: now}); return true;
    },
    pending() { return [...jobs.values()].map(({id, url, options}) => ({id, url, options: clone(options)})); },
    removeTarget(target) { users.delete(target); channels.delete(target); },
    assert(expression, message = expression) {
      // Evaluate INSIDE this realm: no JSON serialization of Maps/closures/identity.
      if ((0, eval)(expression) !== true) throw new Error('heap assertion failed: ' + message);
    },
    throws(action, pattern) {
      let error; try { action(); } catch (caught) { error = caught; }
      if (!error || (pattern && !pattern.test(String(error)))) throw new Error('expected exception');
    },
    checkpoint() { throw new Error('snapshot adapter unavailable: no JSON persistence emulation'); },
  });
})();
const ircd = replay.ircd;
