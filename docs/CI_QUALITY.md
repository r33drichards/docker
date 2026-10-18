# Formatting and static checks

Run `sh tests/ci/quality.sh` from the repository root, or use its absolute path
from another directory. The script is read-only and requires every tool below.
Missing tools, incompatible versions, formatting
differences, lint findings, or stale generated bindings fail the job. There are
no successful skips.

The quality gate is intentionally below `tests/ci/`: the repository's legacy
`run_tests.sh` discovers every top-level `tests/*.sh` as a Docker integration
test. This gate belongs to its dedicated CI job.

## Toolchain

The CI platform is Debian 13 (`debian:trixie`). Its distro packages supply:

- clang-format 19 (locally verified: 19.1.7)
- Cppcheck 2.17.1
- ShellCheck 0.10.0
- Python 3.10 or newer

Ruff is pinned to **0.16.9** in both `.ruff.toml` and the check script. The
workflow must install this exact version. The expected versions are checked
before any source checks run; an accidental PATH/toolchain change fails early.

Install on Debian 13:

```sh
apt-get update
apt-get install -y --no-install-recommends \
    clang-format-19 cppcheck shellcheck python3-venv
python3 -m venv /tmp/irc-quality-venv
/tmp/irc-quality-venv/bin/pip install ruff==0.16.9
PATH="/tmp/irc-quality-venv/bin:$PATH" sh tests/ci/quality.sh
```

Or run the complete gate in an isolated container from the repository root:

```sh
docker run --rm -v "$PWD:/repo:ro" -w /repo debian:trixie sh -ec '
    apt-get update
    apt-get install -y --no-install-recommends \
        ca-certificates clang-format-19 cppcheck shellcheck python3-venv
    python3 -m venv /tmp/quality-venv
    /tmp/quality-venv/bin/pip install ruff==0.16.9
    PATH="/tmp/quality-venv/bin:$PATH" sh tests/ci/quality.sh
'
```

`CLANG_FORMAT`, `RUFF`, `SHELLCHECK`, `CPPCHECK`, and `PYTHON` can each select an
alternate executable path for local development. They do not disable checks or
relax the version requirements. Distro packaging revisions may change while
preserving the required tool version; an actual tool upgrade should update and
verify the pins, configuration, and formatting together.

## Scope and checks

The gate names the new module sources, owned headers, native test programs,
binding generator, Python integration tests, and module-specific shell runners.
It also includes all C++/Python/shell sources below `tests/fuzz/`, Python/shell
scripts below `tests/ci/`, and the heavy-job authorization tests. The existing
unrelated Docker configuration and legacy tests are not reformatted or linted
by this gate. When adding another
module source or top-level test, add it to the explicit list in
`tests/ci/quality.sh`.

1. **C++ format:** `clang-format-19 --dry-run --Werror` enforces the checked-in
   `.clang-format`: tabs, Allman braces, C++17, and a 120-column target. Includes
   retain their original order, including the required daemon header first.
2. **C++ static analysis:** Cppcheck parses owned `.h` and `.cpp` files as C++17,
   with warning, performance, portability, and error checks at exhaustive
   analysis level. Findings fail the job. System headers and external
   InspIRCd/V8 implementation code are outside this source-only analysis;
   complete native compiler and sanitizer builds are separate jobs. Narrow,
   explained inline suppressions are permitted for proven analyzer limitations.
3. **Python lint:** Ruff checks syntax errors, undefined/unused names, import
   order, common bug patterns, and Python-version compatibility improvements.
4. **Python format:** Ruff's formatter checks all scoped Python files against
   the pinned version and 100-column configuration. Lint and format are distinct
   checks; compilation alone is not used as a formatter substitute.
5. **Shell lint:** ShellCheck runs at `style` severity, including its warnings
   and errors. Scripts are parsed according to their declared shell.
6. **Generator drift and contracts:** the generator's `--check` mode compares
   every generated file byte-for-byte, then its unit tests exercise validation
   and deterministic output. `modules/js_generated/` is excluded from generic
   formatting because the generator is its canonical formatter.
7. **Heavy-job selector tests:** positive and negative mocked API cases cover
   current heads, labels, actor permissions, bounded history, and API failures.
   The helper is contributor-editable PR code, so it is a convenience selector,
   not a security boundary. Heavy jobs must remain unprivileged and secret-free.

Ruff caching and Python bytecode writes are disabled in the gate so it works
with a read-only checkout. The checks never download or install dependencies.

## Fixing a failure

Apply the formatter only to the failing owned files, then rerun the whole gate:

```sh
clang-format-19 -i path/to/failing.cpp path/to/failing.h
ruff check --fix path/to/failing.py
ruff format path/to/failing.py
sh tests/ci/quality.sh
```

Review every lint fix. Do not reformat the entire inherited repository or hide
new findings behind broad suppressions. Fix ShellCheck and Cppcheck findings in
the source; keep any necessary suppression next to the code with its reason.
After intentional manifest/generator changes, regenerate with
`python3 tools/generate_js_bindings.py`, review the generated diff, and run
`sh tests/js_bindings.sh` before the aggregate quality gate.

References: [clang-format style options](https://releases.llvm.org/19.1.0/tools/clang/docs/ClangFormatStyleOptions.html),
[Ruff 0.16.9](https://pypi.org/project/ruff/0.16.9/),
[Debian Cppcheck package](https://packages.debian.org/trixie/cppcheck), and
[ShellCheck](https://www.shellcheck.net/).
