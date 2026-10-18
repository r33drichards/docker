# Continuous integration

Every pull-request update runs the quick suite: formatting and static checks,
generator/label-selector unit tests, ASan/UBSan ownership and V8 tests, native
Valgrind ownership tests, three 10-second libFuzzer targets, and actual module
compilation against pinned InspIRCd 4.8.0 and 4.12.1 commits.

## Request heavy checks

A repository writer, maintainer, or administrator can apply the **ci:heavy**
label to a pull request. This additionally runs:
- 120 seconds per fuzz target;
- Valgrind V8 lifecycle checks across repeated module reloads;
- Alpine runtime and real-daemon integration tests on both supported versions;
- the repository's existing default-image build/test workflow.

The label remains effective on subsequent commits. Remove it to cancel the
current heavy run and stop requesting new heavy runs; reapply it to request a
fresh run. Unrelated label edits neither restart nor cancel heavy runs.
Per-PR concurrency cancels superseded runs. Always inspect checks for the latest
head SHA: a cancelled or skipped check is not evidence that the heavy suite
passed. There are no scheduled heavy runs.

The JavaScript selector rechecks the current head, current label, and the most
recent label actor's repository permission. Missing history or API errors fail
closed. The default-image workflow uses the label directly. These checks are
resource-management conveniences, not an immutable security boundary: PR authors
can edit PR workflows and scripts. GitHub's fork-workflow approval controls remain
the execution boundary. All PR jobs use hosted runners and read-only tokens,
without deployment credentials, privileged pull_request_target execution, or
image publication. Existing upstream master publishing remains separately guarded.

## Reproduce and diagnose

See [quality tools](CI_QUALITY.md), [native instrumentation](CI_NATIVE.md), and
[fuzzing](../tests/fuzz/README.md) for local commands and target coverage.
Missing dependencies and tool failures are errors, not skipped tests.
Permission tests run as non-root. Leak detection cannot be disabled in CI.

Action revisions are full-SHA pinned. Clang/clang-format 19 and GCC 14 are selected
explicitly; Ruff and actionlint are exact-version pinned, and actionlint's archive
is SHA-256 verified. Debian trixie package security revisions can advance; native
jobs record installed versions. Optional Alpine builds use the Dockerfile's
matched, version-pinned Node/V8 development and runtime packages. These are not
claims of bit-for-bit reproducible toolchains.

Pip and per-version Docker build caches reduce download/build work. Test results
are never restored from caches. Native and smoke-fuzz artifacts are retained for
14 days, extended fuzz/lifecycle artifacts for 30 days. Fuzz artifacts include
binaries, corpora, crash inputs, seed/settings, and logs for replay. A failing
fuzz input should become a regression seed after diagnosis.

ASan/UBSan instrument our native code, not the distribution's prebuilt V8.
V8's RTTI-free ABI requires disabling only UBSan's vptr check in that target.
Valgrind's V8 lifecycle lane uses jitless execution and no suppression files,
including distribution default suppressions. A warmed-up process-lifetime V8
baseline is expected; the test requires zero growth across reloads and zero
invalid accesses or definitely/indirectly lost bytes. Normal JIT/Wasm execution
is covered separately. These checks do not promise hard resource isolation or
prove the entire upstream engine memory-safe.

References: [GitHub workflow security](https://docs.github.com/en/actions/reference/security/secure-use),
[concurrency](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/control-workflow-concurrency),
and [V8 platform lifecycle API](https://chromium.googlesource.com/v8/v8/+/refs/heads/main/include/libplatform/libplatform.h).
