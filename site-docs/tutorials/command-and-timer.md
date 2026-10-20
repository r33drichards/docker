# Register a command and a timer

Use a disposable local server with the native-host API build. This tutorial creates
an operator-only, local command and a main-loop timer. It does not change channel
modes, load modules or contact a remote service.

Create a trusted script `scripts/agent-status.js`:

~~~javascript
let ticks = 0;
ircd.setTimer(10, true, () => { ticks += 1; });
ircd.registerCommand('AGENTSTATUS', 0, 0, true, event => {
  const user = ircd.userByNick('example-agent');
  const suffix = user ? ' example-agent is present' : '';
  ircd.notice(event.userId, 'timer ticks=' + ticks + suffix);
});
~~~

Load it in an isolated configuration:

~~~text
<module name="javascript">
<javascriptsettings timeoutms="25">
<javascript file="scripts/agent-status.js" authority="admin">
~~~

Connect as an operator using your existing test credentials and issue `/AGENTSTATUS`.
Use a unique command name: registrations cannot replace existing native commands or
commands owned by another script. There are at most 32 commands per script, names are
1–32 uppercase ASCII letters/digits, and parameter bounds are integers from 0 to 15.
The `true` argument restricts invocation to operators; it does not grant operator status.

After ten seconds, the count increases. Missed timer intervals are skipped rather
than replayed in a burst. Timer intervals are whole seconds from 1 to 86400, with at
most 32 active timers per script. `clearTimer(id)` cancels a returned timer ID.

A successful reload creates a fresh `ticks` value and new timer. A failed candidate
load preserves the old script. Test failure and reload cases with the
[replay framework](../how-to/replay.md), then use the native integration harness to
verify actual registration, permissions and lifecycle.
