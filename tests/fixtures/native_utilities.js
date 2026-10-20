// Initialization-time assertions for the expanded primitive host utility surface.
(function () {
  function check(value, name) { if (!value) throw Error('utility: ' + name); }
  function rejects(callback, name) {
    let rejected = false;
    try { callback(); } catch (_) { rejected = true; }
    check(rejected, name);
  }
  check(ircd.isUser('user') && !ircd.isUser('bad user'), 'username');
  check(ircd.defaultIsUser('user') && ircd.defaultIsNick('nick'), 'default validators');
  check(ircd.isHost('localhost', true) && !ircd.isHost('localhost', false), 'hostname policy');
  check(ircd.isFQDN('example.com') && !ircd.isFQDN('localhost'), 'fqdn');
  check(ircd.isSID('001') && !ircd.isSID('xx'), 'sid');
  check(ircd.isValidMask('nick!user@example.com'), 'mask');
  check(ircd.matchMask('*.example.com', 'a.example.com', '192.0.2.1'), 'mask match');
  check(ircd.processColors('plain') === 'plain', 'color processing');
  check(ircd.timingSafeCompare('a\0b', 'a\0b') && !ircd.timingSafeCompare('a\0b', 'a\0c'), 'nul compare');
  check(!ircd.timingSafeCompare('a', 'aa'), 'length compare');
  check(ircd.randomInt(1) === 0, 'exclusive upper bound');
  for (let i = 0; i < 32; ++i) {
    const n = ircd.randomInt(0xffffffff);
    check(Number.isInteger(n) && n >= 0 && n < 0xffffffff, 'random uint32');
  }
  check(ircd.randomString(0) === '' && /^[A-Za-z0-9]{64}$/.test(ircd.randomString(64)), 'random string');
  check(ircd.timeNanoseconds() >= 0 && ircd.timeNanoseconds() < 1000000000, 'time component');
  rejects(() => ircd.randomInt(0), 'zero bound');
  rejects(() => ircd.randomInt(-1), 'negative bound');
  rejects(() => ircd.randomInt(1.5), 'fractional bound');
  rejects(() => ircd.randomInt(2 ** 32), 'wide bound');
  rejects(() => ircd.randomInt('2'), 'integer coercion');
  rejects(() => ircd.randomString(4097), 'allocation bound');
  rejects(() => ircd.isHost('example.com', 1), 'boolean coercion');
  rejects(() => ircd.isUser('\ud800'), 'malformed utf16');
  rejects(() => ircd.isUser('a\0b'), 'nul validator');
  rejects(() => ircd.matchMask('x'.repeat(4097), 'host', 'ip'), 'input limit');
  rejects(() => ircd.timeNanoseconds(1), 'exact arity');
})();
