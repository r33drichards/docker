# Native memory-safety CI

Run from any working directory:

```sh
bash tests/ci/native.sh sanitizers
bash tests/ci/native.sh valgrind
bash tests/ci/native.sh lifecycle
# All three, with every dependency required:
bash tests/ci/native.sh all
```

These are explicit CI entry points, separate from the legacy container test
runner's `tests/*.sh` discovery. Missing compilers, V8, Valgrind, debug symbols,
headers, or a timed-out/failed test make the requested lane fail; there is no
successful dependency skip. Run as a non-root user so the source-loader tests
actually exercise unreadable files and directories.

## Toolchain and jobs

The native CI environment is the official `debian:trixie` image, with `g++-14`,
`libnode-dev`, `valgrind`, `libc6-dbg`, `binutils`, and `coreutils` from Debian's
package repositories. Match the V8 headers and shared library from the same
package installation. The compiler major is pinned; distribution security patch
versions may advance. The lifecycle lane requires GCC because it deliberately
uses `-fno-gnu-unique`. The independent lint/fuzz jobs use their own pinned tools.
Install dependencies as root, then switch to an ordinary user for these tests.

The quick CI set runs ASan/UBSan and non-V8 ownership Memcheck. The opt-in
`ci:heavy` set also runs the NODELETE lifecycle check and longer fuzzing. These
native tests do not replace the separate Alpine image build/integration tests:
Debian/glibc libraries must never be copied into the Alpine/musl image.

## What each lane verifies

| Lane | Actual code executed | Failure conditions |
| --- | --- | --- |
| `sanitizers` | Existing `precommand_struct.cpp`, `js_script_loader.cpp`, and `js_runtime.cpp`, with the module headers compiled into the tests | AddressSanitizer, LeakSanitizer, UBSan, assertions, compile failure, or timeout |
| `valgrind` | The same command and source-loader suites, compiled without sanitizers | Any Memcheck error or any leak kind, including still-reachable bytes |
| `lifecycle` | A hidden-symbol, explicitly NODELETE shared object containing the real `Runtime`, repeatedly opened/closed by a small host | Missing NODELETE, state reset/unmapping, invalid accesses/undefined values, definite/indirect leaks, or growth in possible/reachable heap bytes after warmup |

The ownership suites cover retained command buffers, all-byte representation,
owned JS/Wasm bytes, canonical paths, duplicate detection, symlinks, FIFOs,
permission errors, exact byte/count limits, and failed-candidate isolation.
The sanitizer runtime suite exercises isolate separation, persistent/context
lifetime, errors, promises, WebAssembly validation/traps, operation deadlines,
reentrancy, owner-thread rejection, repeated recreation, and disposal after
termination. These are targeted tests, not a proof of whole-server memory safety.

### ASan/UBSan scope

Application and inline embedder code are built with
`-fsanitize=address,undefined -fno-sanitize-recover=all`, frame pointers, and debug
information. Runtime options stop at the first error and keep leak detection on.
Only the V8-linked runtime test adds `-fno-sanitize=vptr`: distro V8 is built
without the RTTI symbols that this check would require. Other UBSan checks stay
on; the independent command/loader binaries retain the vptr check too.

The packaged V8 library and its generated machine code are **not rebuilt with
sanitizer instrumentation**. Interceptors can catch some external-library
misuses, but passing these tests does not establish sanitizer coverage of V8
internals or JIT-generated code. See the official [ASan documentation](https://clang.llvm.org/docs/AddressSanitizer.html)
and [UBSan check list](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html).

### Valgrind and V8 lifetime

The lifecycle module uses `--jitless --no-expose-wasm` before initializing V8.
This isolates native embedding/lifetime behavior from generated code; the normal
sanitizer runtime suite still uses its regular JS/WebAssembly execution paths.
V8 documents the different execution mode in [JIT-less V8](https://v8.dev/blog/jitless).
Memcheck retains its uninitialized-value checks and origin tracking. Distro
default suppressions are explicitly disabled, and no project suppression file or
broad V8 suppression is installed. The explicit
`--smc-check=all-non-file` setting follows Valgrind's
[self-modifying-code guidance](https://valgrind.org/docs/manual/manual-core.html#manual-core.adv).

The Memcheck command requires `--fair-sched=yes`. Valgrind's default pipe-based
scheduler can let a busy interpreter repeatedly reacquire its execution lock,
starving the watchdog that must interrupt the infinite-script test. This was
reproduced on an unpinned multicore run: the default scheduler stalled during a
warmup timeout, while the same binary with fair scheduling completed all 28
generations in about 13 seconds. The [official threading guidance](https://valgrind.org/docs/manual/manual-core.html#manual-core.pthreads)
explains this fairness difference. Fair scheduling changes the test tool's
scheduling, not the runtime's deadlines or thread count; it is required rather
than allowed to fall back silently. The 180-second outer cap and every memory
and lifecycle assertion remain unchanged.

The host and module emit flushed, monotonic-timestamped phase diagnostics before
and after loading, construction, script execution, destruction, and heap scans.
Application output streams live to the CI console and is retained in the stdout
log. Valgrind also writes ten-second CPU/thread progress reports and timestamped
errors to its own log, so a timeout identifies the last running phase rather
than producing only an empty warmup log.

The platform is deliberately process-lived and the DSO deliberately NODELETE.
Consequently, a live worker thread's TLS and reachable platform allocations
remain at process exit. The V8 lane does **not** claim zero exit allocations.
Its host warms up four generations, snapshots Memcheck's byte counts, runs 24
more generations, and requires no increase in possible or reachable bytes, no
definite/indirectly lost bytes, and no suppressed heap bytes. Every generation
creates/destroys successful, failed, terminated, and fresh runtimes; caller-owned
persistent handles are reset before destruction. The exported generation
counter and `RTLD_NOLOAD` check verify retention across actual `dlclose` calls.
`-fno-gnu-unique` prevents implicit GNU-unique-symbol retention from making a
missing explicit NODELETE flag appear correct.

The command/loader Memcheck lane has no process-lifetime exemption and fails on
all leak kinds. Valgrind's documented distinction between
[reachable, possible, and lost allocations](https://valgrind.org/docs/manual/mc-manual.html#mc-manual.leaks)
explains the two acceptance policies. Baseline totals and full error diagnostics
are saved, not silently suppressed.

The real runtime notifies the retained default platform before disposing each
isolate. V8's [libplatform API contract](https://chromium.googlesource.com/v8/v8/+/refs/heads/main/include/libplatform/libplatform.h)
requires `NotifyIsolateShutdown` when the platform itself is not shutting down.
Without that notification, foreground task queues can remain reachable for each
disposed isolate; the no-growth assertion is intended to catch this regression.

## Logs, bounds, and local reproduction

Each compile and test has a 180-second limit, with a ten-second forced-kill grace
period. `CI_NATIVE_TIMEOUT` and `CI_NATIVE_BUILD_TIMEOUT` accept positive integer
seconds for slower diagnostic environments. Preserve `test-results/native/` as
CI artifacts even on failure. Each mode writes compiler/environment information,
compile commands, test output, and Memcheck reports under its own subdirectory.
Temporary binaries and fixtures are removed on exit.

Supported environment variables:

- `CXX`: compiler executable, default `g++`; use `g++-14` in pinned native CI.
- `CI_NATIVE_LOG_DIR`: output directory for persistent logs.
- `V8_CXXFLAGS` / `V8_LDFLAGS`: whitespace-separated matched include/link flags.
  Defaults use `pkg-config libnode`, then `/usr/include/node` and `-lnode`.
- `VALGRIND`: Valgrind executable, default `valgrind`.
- `VALGRIND_CXXFLAGS`: alternate Valgrind-header flags for the lifecycle host.
- `VALGRIND_EXTRA_FLAGS`: local diagnostic flags, such as an extracted glibc
  debug-info path. Do not use this to weaken CI's acceptance policy.
- `LD_LIBRARY_PATH` / `VALGRIND_LIB`: standard runtime paths for locally extracted
  matched packages. Use these only when packages are not normally installed.

Flag-list variables do not evaluate shell code and cannot encode whitespace
inside individual path arguments. A local extracted-package example is:

```sh
export V8_CXXFLAGS='-isystem /path/sysroot/usr/include/node'
export V8_LDFLAGS='-L/path/sysroot/usr/lib/x86_64-linux-gnu -Wl,-rpath,/path/sysroot/usr/lib/x86_64-linux-gnu -lnode'
export LD_LIBRARY_PATH=/path/sysroot/usr/lib/x86_64-linux-gnu
CXX=g++-14 bash tests/ci/native.sh all
```

LeakSanitizer cannot inspect threads under some ptrace-based executor sandboxes.
Do not work around that in CI. On such a local executor only,
`CI_NATIVE_ALLOW_NO_LSAN=1` runs ASan/UBSan with an explicit warning and saved
`lsan-disabled.log`; it is rejected whenever `CI` is set to a value other than
`false`. This partial local run is not a LeakSanitizer pass. Use the normal CI
lane to obtain leak-check evidence. The initial local implementation was tested
on Debian 13/GCC 14/V8 11.3 with this limitation; actual Memcheck ownership and
lifecycle results provide separate leak/lifetime evidence.
