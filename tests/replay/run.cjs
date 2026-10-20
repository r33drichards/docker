/* SPDX-License-Identifier: MIT */
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const args = process.argv.slice(2);
let target;
const preloads = [];
while (args[0]?.startsWith('--')) {
  const option = args.shift(), value = args.shift();
  if (!value) throw new Error(option + ' needs a path');
  if (option === '--script') target = value;
  else if (option === '--preload') preloads.push(value);
  else throw new Error('unknown replay option: ' + option);
}
const files = args.length ? args : fs.readdirSync(path.join(__dirname, 'scenarios')).filter(f => f.endsWith('.js')).sort().map(f => path.join(__dirname, 'scenarios', f));
for (const file of files) {
  const context = vm.createContext(Object.create(null), {codeGeneration: {strings: true, wasm: false}});
  for (const source of [...preloads, path.join(__dirname, 'host.js'), ...(target ? [target] : []), file]) {
    vm.runInContext(fs.readFileSync(source, 'utf8'), context, {filename: source, timeout: 1000});
  }
  console.log('PASS replay ' + path.basename(file));
}
