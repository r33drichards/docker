// Trusted administrator-authored tutorial code; local disposable server only.
ircd.on('preCommand', event => {
  if (event.validated) return; // Rewrites run only before native validation.
  if (event.command === 'PING' && event.params[0] === 'lesson')
    return { action: 'pass', command: 'PING', params: ['hello-from-javascript'] };
});
ircd.on('postCommand', event => {
  if (event.command === 'VERSION')
    ircd.notice(event.userId, 'VERSION handler returned ' + event.result);
});
