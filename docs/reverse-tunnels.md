# Reverse SSH tunnels (InspIRCd 4.12.1, Linux)

`reverse_tunnel` is an opt-in command and configuration API. It supervises an
OpenSSH companion; it does not implement SSH or modify IRC listeners. Loading
it with no tunnel entries starts no connections. The image ships the module,
`/usr/local/libexec/inspircd-tunnel`, and the Alpine OpenSSH client. It is not
loaded by the default configuration.

## Configuration

Mount a configuration file under `/inspircd/conf.d/` (or include it from your own
configuration). Mount dedicated credential files read-only, accessible to UID
10000. Provision host keys through a separately verified trusted channel. Never
put private key contents, passwords, or passphrases in IRC commands/config tags.
The module never reads private keys: OpenSSH opens the referenced file.

```xml
<module name="reverse_tunnel">
<tunnelprofile name="edge"
  host="gateway.example.net" port="22" user="irc-tunnel"
  identityfile="/run/secrets/tunnel_identity"
  knownhostsfile="/run/secrets/tunnel_known_hosts"
  destination="127.0.0.1"
  listenmin="16667" listenmax="16669"
  destmin="6667" destmax="6667">
<reversetunnel name="irc" profile="edge"
  listenport="16667" destinationport="6667" state="enabled">
```

`state` is `enabled` (the default) or `disabled`. Disabled entries retain their
configuration and ownership but must have no running tunnel. They still reserve
their listener tuple and must pass the same validation, so enabling them cannot
silently collide with another desired entry.

This requests `127.0.0.1:16667:127.0.0.1:6667` on the SSH gateway. Destination
addresses are resolved from the IRC container; listen addresses are on the SSH
server. Profiles are admin-owned capability boundaries: a dynamic command can
choose an existing profile and ports within its explicit inclusive ranges.
Profile port bounds are required, remote listen ports must be 1024–65535, and
destination/SSH ports must be 1–65535. Use the smallest useful bounds.

The first version permits only IPv4/DNS destinations and SSH hosts and a fixed
IPv4 loopback remote bind. IPv6, public binds, arbitrary SSH options, interactive
authentication and dynamic profile creation are intentionally outside its API.
Host strings are limited to 253 ASCII letters/digits/dots/hyphens; profile/tunnel
names and SSH usernames to 32 ASCII letters/digits/underscores/hyphens (no leading
hyphen). Absolute file references permit only letters/digits, `/`, `_`, `.`, `-`
and at most 240 characters. `%` and `~` expansion is disallowed. There are at
most 32 profiles and 32 tunnels. Duplicate remote host/SSH-port/listen-port tuples
are rejected; aliases for the same host cannot be detected locally.

OpenSSH uses `-F /dev/null`, a minimal environment, strict known-host checking,
the specified known-hosts file, no global known-hosts file, no agent, no password
or keyboard-interactive authentication, and the specified identity. It disables
control sockets, local commands and agent forwarding. No shell constructs are
used; all arguments are passed as an argv vector. SSH stdin/stdout/stderr go to
`/dev/null`, so errors or key material cannot leak into IRC or daemon logs.

The gateway administrator must keep `GatewayPorts no` (an SSH server can override
requested bind addresses), restrict the account to remote forwarding and permitted
listen endpoints, and deny shell/PTY access. DNS and gateway policy remain trust
boundaries. The module does not configure the gateway or firewall. A dedicated
key/account and read-only host-pinning file are recommended. No tunnel is enabled
or gateway changed by installing this module.

## Operator commands

Grant both the `RTUNNEL` command and the `servers/reverse-tunnel` privilege in the
operator's existing `<class>`; operators with wildcard permissions also qualify.
Only locally connected operators can use the command; it is never routed to
other servers. Names and verbs are case-sensitive.

```text
/RTUNNEL LIST
/RTUNNEL SHOW irc
/RTUNNEL ADD temporary edge 16668 6667 disabled
/RTUNNEL ENABLE temporary
/RTUNNEL DISABLE temporary
/RTUNNEL UPDATE temporary edge 16669 6667 enabled
/RTUNNEL DELETE temporary
```

ADD and UPDATE accept an optional final `enabled|disabled` argument. ADD defaults
to enabled; UPDATE preserves the previous desired state when omitted. ENABLE and
DISABLE are shortcuts for changing only that field. DELETE removes desired state.
All mutations only validate and submit desired state; the background timer performs
process reconciliation. An accepted reply is not confirmation that the process has
already started or stopped. Dynamic state changes, including disabled entries,
remain memory-only.

All operations, including LIST/SHOW, require the operator privilege. Status includes
name, `owner=config|dynamic`, `desired=enabled|disabled`, desired `generation`,
`observed-generation`, observed `state`, reconciliation progress, capped failure
count, retry delay in seconds and a fixed failure category. It never returns
endpoints, credential paths, usernames, child PIDs or SSH diagnostics. Example:

```text
temporary owner=dynamic desired=disabled generation=2 observed-generation=1 state=stopping reconcile=pending failures=0 retry-in=0 last-failure=none
```

States are `pending`, `starting`, `ssh-running-unverified`, `ssh-exited`,
`spawn-failed`, `backoff`, `stopping` and `stopped`. `observed-generation=0` means
there is no owned companion; an enabled entry is pending, while a disabled entry
is stopped. A pending reconciliation can briefly show the previous generation's
running state. Deleted entries disappear from LIST/SHOW, but their retiring
companions remain controller-owned until cleanup finishes.

Running means a child was launched; it is **not a connection or remote-listener
health check**. OpenSSH uses `ExitOnForwardFailure`, a 10-second connect timeout,
one connection attempt and 15-second keepalives with three misses. A rejected
key or busy remote port causes controller retries rather than a false connected
report. Inspect gateway logs separately for diagnosis.

## Ownership, rehash and persistence

Config-file and dynamic entries use one `Tunnel` model, validator and lifecycle.
Only dynamic entries can be updated/deleted by commands. Config-owned entries
must be edited in the file and rehashed. Rehash stages and validates the full
candidate before changing module state. Invalid entries, duplicate names,
listener collisions and config/dynamic name conflicts leave the module's
previous desired state and processes intact (other modules' rehash behavior is
independent).

Every effective desired-state change gets a new monotonically increasing
module-lifetime generation. An unchanged submission/rehash retains the generation,
companion and backoff deadline. The background timer compares desired and observed
state, restores missing enabled companions, and retires disabled, deleted or stale
generations. Profile changes restart affected entries when their effective SSH argv
changes. It does not perform network operations or wait for processes.

Replacement waits for the old companion's socket EOF before starting the next
generation of that name. Late status from a retiring generation is ignored; only
its EOF matters. Delete/recreate and rapid disable/enable before a tick still get
new generations and cannot inherit the old process or its retry. Disabled/deleted
entries cannot schedule recovery. Retiring companions count towards the limit of
32 owned companions, so repeated edits cannot create unbounded processes.

Dynamic entries survive rehash, including state and port-policy revalidation.
Removing their profile or narrowing bounds to exclude them rejects the module's
rehash. Delete those entries first. Moving a dynamic name into config also requires
deleting it first.

**Dynamic changes are memory-only.** They are not written into the configuration
or a database and disappear on module unload/reload or daemon restart. To retain
a tunnel, add a declarative entry to your managed config. Credentials are always
file references and are never persisted by this module. Rotation at the same
path is picked up by OpenSSH on its next connection; unchanged rehash does not
force reconnect. Disable then enable the entry (or remove/re-add it) to request a
new generation. Config-owned entries require the corresponding edits and rehashes;
reloading the module also drops dynamic entries.

## Process lifecycle

InspIRCd ignores `SIGCHLD`, so the module never retains or signals a helper PID.
A private close-on-exec socket ties each companion to one observed generation.
The one-shot companion restores normal child reaping and owns exactly one SSH
child; it never reconnects or retries autonomously. The controller starts a new
companion when an enabled entry needs recovery.

On the next background tick, delete, disable or replacement half-closes the
control socket. The companion detects closure within its 200 ms poll interval,
kills and reaps its child, and exits. The controller retains the read side until
EOF, without blocking, before retiring that generation. Unload/destruction and
daemon exit close every socket directly and cancel all desired state. Linux
parent-death signaling kills SSH if its companion crashes. No cleanup operation
can accidentally signal a recycled PID. Kernel-uninterruptible processes can
delay OS cleanup; a slow retiring generation does not allow an overlapping
replacement of the same name.

All retries are scheduled by the controller using monotonic time and exponential
backoff of 1, 2, 4, 8, 16, 32, then 64 seconds, capped indefinitely while enabled.
A companion surviving 60 seconds resets the failure count on its next failure.
SSH exit, missing companion and spawn failure all use that same retry policy.
Reconciliation runs on InspIRCd's five-second background tick, so observation,
stopping, retry and replacement can occur up to a tick after becoming eligible.
Actual retry intervals can therefore exceed the nominal backoff. Idempotent
submissions do not reset failure counters or accelerate retries.

Start forks a tiny exec stub which closes inherited descriptors; networking,
authentication, keepalive and child reaping stay outside the daemon. The timer
only validates ownership, performs bounded nonblocking socket reads, launches
eligible companions and requests cleanup. Unit tests inject a fake transport
and a monotonic clock to exercise these scheduling guarantees deterministically.

## Verification

`tests/test_reverse_tunnel.cpp` tests validation, merge rules, limits, SSH argv
and backoff without a daemon. `tests/test_reverse_tunnel_controller.cpp` tests
desired/observed separation, generations, stale events, kill/loss recovery,
idempotent rehash, backoff, disable/delete cancellation and unload with a fake
transport. `tests/reverse_tunnel.sh` also builds an isolated
integration image from `inspircd:testing`; `run_tests.sh` discovers it in CI.
It runs the real module and companion with a synthetic SSH executable under
`docker run --network none`. It verifies permission denial, config and dynamic
CRUD, redacted status, rehash atomicity, preservation of unchanged processes,
reconnect delays, disable/delete without resurrection, unload/reload volatility and daemon-crash cleanup.
The synthetic test uses no credentials or external SSH/IRC infrastructure and
exercises the supervision contract deterministically.

The same runner also builds `tests/reverse-tunnel/Dockerfile.real` and runs a
second disposable container with `--network none` and no host mounts. This uses
the real module, companion, OpenSSH client and OpenSSH server with fresh Ed25519
test keys generated inside the container. It verifies encrypted bidirectional
payload forwarding through the remote loopback listener, rejection of a wrong
pinned server key before authentication, recovery after an SSH server disconnect
and restart, recovery after killing the real SSH child, declarative disable/enable
through rehash without resurrection, and removal of the real SSH client and
listener on unload and daemon crash. Only loopback is permitted to be UP; all services bind to loopback. Python
reads only fixture public keys; OpenSSH alone opens fixture private keys. Docker
removes the container and its keys after the test. The SSH server and temporary
test account are present only in this test image, never in the production image.

These tests cover real encrypted forwarding within an isolated network namespace;
they do not validate a production gateway, DNS, firewall or external connectivity.

```sh
docker build -t inspircd:testing .
sh tests/reverse_tunnel.sh
```
