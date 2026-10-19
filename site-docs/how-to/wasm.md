# Call a configured WebAssembly asset

Use this guide to call a synchronous Wasm export from a JavaScript handler.

1. Copy the repository's `examples/javascript/answer.wasm` into your configuration
   directory as `scripts/answer.wasm`. The adjacent `answer.wat` documents its i32
   `answer()` export, which returns 42.
2. Configure the asset and your JavaScript file:

   ~~~text
   <javascriptwasm name="answer" file="scripts/answer.wasm">
   <javascript file="scripts/wasm.js" authority="user">
   ~~~

3. Use synchronous compilation and instantiation in `scripts/wasm.js`:

   ~~~js
   const instance = new WebAssembly.Instance(
     new WebAssembly.Module(ircd.wasm('answer')), {});
   ircd.on('preCommand', event => {
     if (event.command === 'PING' && event.params[0] === 'wasm')
       return { action: 'pass', command: 'PING',
                params: [String(instance.exports.answer())] };
   });
   ~~~

4. Rehash and send a raw `PING :wasm` on a disposable server. Expect a PONG ending
   in `42`. Check load/runtime logs if compilation or execution fails.

There is no direct Wasm IRC ABI, implicit import resolution, WASI, network or
filesystem API. Supply imports explicitly; calls inherit the script's configured
authority and deadline. Async compilation/Promises are unsupported.
