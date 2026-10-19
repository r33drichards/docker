// Trusted administrator code. This file runs in its own persistent isolate.
// A pre callback returns undefined, { action: 'deny' }, or
// { action: 'pass', command?: string, params?: (string | Uint8Array)[] }.
ircd.on('preCommand', event => {
  if (event.command === 'EXAMPLEPING') {
    return { action: 'pass', command: 'PING', params: ['hello from JavaScript'] };
  }
});

ircd.on('postCommand', event => {
  if (event.command === 'VERSION') {
    ircd.notice(event.userId, 'VERSION handler returned ' + event.result);
  }
});
