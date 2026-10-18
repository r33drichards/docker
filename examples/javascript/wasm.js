// Configure <javascriptwasm name="answer" file="scripts/answer.wasm">.
// These synchronous APIs run inside the same deadline as initialization/hooks.
const module = new WebAssembly.Module(ircd.wasm('answer'));
const instance = new WebAssembly.Instance(module, {});
ircd.on('preCommand', event => {
  if (event.command === 'WASMANSWER') {
    return { action: 'pass', command: 'PING', params: [String(instance.exports.answer())] };
  }
});
