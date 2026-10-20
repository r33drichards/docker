/* SPDX-License-Identifier: MIT */
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const args = process.argv.slice(2);
let target;
if (args[0] === '--script') { args.shift(); target = args.shift(); if (!target) throw new Error('--script needs a path'); }
const files = args.length ? args : fs.readdirSync(path.join(__dirname, 'scenarios')).filter(f => f.endsWith('.js')).sort().map(f => path.join(__dirname, 'scenarios', f));
for (const file of files) {
  const context = vm.createContext(Object.create(null), {codeGeneration: {strings: true, wasm: false}});
  for (const source of [path.join(__dirname, 'host.js'), ...(target ? [target] : []), file]) {
    vm.runInContext(fs.readFileSync(source, 'utf8'), context, {filename: source, timeout: 1000});
  }
  console.log('PASS replay ' + path.basename(file));
}
