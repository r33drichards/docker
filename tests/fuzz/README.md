# Bounded native fuzzing

These are actual coverage-guided libFuzzer targets, compiled with Clang's
`-fsanitize=fuzzer,address,undefined`, rather than randomly generated unit tests.
No InspIRCd server, V8 isolate, network service, or Docker daemon is required.

## Run

On Debian trixie, install the official `clang-19`, `libclang-rt-19-dev`, `python3`
and `coreutils` packages. Missing tools or sanitizer runtimes fail the run;
there is no silent skip or fallback to an unsanitized build.

```sh
tests/fuzz/run.sh
FUZZ_OUT=/tmp/irc-fuzz-heavy FUZZ_SECONDS=120 tests/fuzz/run.sh
```

The ordinary PR budget is 10 seconds per target (about 30 seconds of fuzzing,
plus compiler startup). The `ci:heavy` CI label selects 120 seconds per target.
`FUZZ_SECONDS` must be 1–3600. `CXX` may select another compatible Clang binary;
the project CI uses `clang++-19`. `FUZZ_SEED` defaults to `20261018` and is recorded
with the compiler version. Time-bounded runs are not guaranteed to execute the
same number of mutations; saved artifact inputs are the precise reproducers.

Every target has an engine time limit, a five-second input timeout, 4096-byte
maximum input, 1024 MiB RSS limit, and 128 MiB single-allocation limit. A separate
`timeout` watchdog fails stalled compiler or fuzz processes. ASan leak detection
and nonrecovering UBSan are enabled. The loader touches only a private temporary
directory, a fixed small set of files/symlinks and one nonblocking FIFO; each
payload is at most 384 bytes. Fuzz bytes never select filesystem paths, launch
scripts, or execute Wasm. Fixture contents are reset before every input and
the directory is removed on normal process exit.

For a local traced/sandboxed process where LeakSanitizer reports its documented
ptrace incompatibility, `FUZZ_DETECT_LEAKS=0` explicitly disables leak checking
only. The runner prints a warning and records that setting. This is not a full
leak-checking pass; CI uses the default `FUZZ_DETECT_LEAKS=1`. ASan memory-access
checks and nonrecovering UBSan remain enabled in either case.

## Targets and oracles

- `wire_validation`: exercises the production `js_wire::Validate` boundary after
  JavaScript conversion. An independent encoder checks command grammar, middle
  parameters, NUL/CR/LF rejection, the 15-parameter limit, and the conservative
  510-byte serialized size. This does not fuzz V8 conversion or IRC redispatch.
- `parsed_command`: decodes the complete `ParsedCommand::Repr()` grammar using
  a separate decoder and checks exact byte preservation, owned snapshot
  lifetime, deterministic output, and printable-ASCII log safety. Arbitrary
  bytes include invalid UTF-8, quotes, backslashes, NUL, CR and LF.
- `script_loader`: compares source/Wasm acceptance and returned data to a small
  independent fixture model: byte/count limits, directory ordering, authority,
  explicit symlinks, ignored entry symlinks/nonfiles, duplicate canonical
  paths/names, missing paths, invalid kinds/names, NUL paths, and FIFO rejection.
  It is single-process and intentionally does not simulate filesystem races;
  the deterministic loader unit suite remains important.

`seeds.json` is the checked-in, human-readable corpus specification. Strings are
UTF-8; `{ "hex": "00ff" }` supplies arbitrary bytes and `repeat`/`count` expresses
boundary lengths without large fixtures. `generate_seeds.py` deterministically
materializes these and named count/every-byte/loader-boundary seeds. Command
inputs use a little-endian u16 length plus bytes, a u8 parameter count modulo 18,
then u16 lengths plus parameter bytes. Loader layout is documented by the
generator and `LLVMFuzzerTestOneInput`; malformed/truncated inputs are valid.

## Failures and reproduction

The runner retains `bin/`, `logs/`, `corpus/`, `compiler.txt`, `settings.txt` and
`artifacts/<target>/` under `FUZZ_OUT` (a printed temporary path by default).
CI should upload this directory even on failure. libFuzzer writes exact
`crash-*`, `leak-*`, `timeout-*`, or `oom-*` inputs before exiting nonzero.

```sh
/path/to/FUZZ_OUT/bin/wire_validation -runs=1 /path/to/artifacts/wire_validation/crash-HASH
```

To replay elsewhere, rebuild using the runner and matching Clang/sanitizers,
then pass the saved artifact file to the corresponding target. Do not pass a
corpus directory for single-case reproduction. Preserve minimized regression
cases in the readable seed specification when fixing a discovered failure.

Primary reference: [LLVM libFuzzer documentation](https://llvm.org/docs/LibFuzzer.html)
(target determinism, focused targets, sanitizer builds, corpus seeding, bounded
options, artifact generation and file-based replay).
