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
  listenport="16667" destinationport="6667">
```

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
/RTUNNEL ADD temporary edge 16668 6667
/RTUNNEL UPDATE temporary edge 16669 6667
/RTUNNEL DELETE temporary
```

All operations, including LIST/SHOW, require that privilege. LIST/SHOW report only
name, `owner=config|dynamic`, and state. They never return endpoints, profile
credentials, paths, usernames, child PIDs or SSH diagnostics. Mutation replies
mean the desired state was accepted, not that a network connection succeeded.

States are `pending`, `starting`, `ssh-running-unverified`, `backoff` and
`companion-backoff`. Running means a child was launched; it is **not a connection
or remote-listener health check**. OpenSSH uses `ExitOnForwardFailure`, a 10-second
connect timeout, one connection attempt and 15-second keepalives with three
misses. A rejected key or busy remote port therefore causes retries rather than
a false connected report. Inspect gateway logs separately for diagnosis.

## Ownership, rehash and persistence

Config-file and dynamic entries use one `Tunnel` model, validator and lifecycle.
Only dynamic entries can be updated/deleted by commands. Config-owned entries
must be edited in the file and rehashed. Rehash stages and validates the full
candidate before changing module state. Invalid entries, duplicate names,
listener collisions and config/dynamic name conflicts leave the module's
previous desired state and processes intact (other modules' rehash behavior is
independent).

Unchanged effective SSH argv retains its companion. Removed or changed entries
close the old companion's control socket; replacements start on the next daemon
background tick. A short overlap while the old process exits can cause a harmless
bind failure and retry. Profile changes restart affected entries. Dynamic entries
survive rehash, including port-policy revalidation; removing their profile or
narrowing bounds to exclude them rejects the module's rehash. Delete those
entries first. Moving a dynamic name into config also requires deleting it first.

**Dynamic changes are memory-only.** They are not written into the configuration
or a database and disappear on module unload/reload or daemon restart. To retain
a tunnel, add a declarative entry to your managed config. Credentials are always
file references and are never persisted by this module. Rotation at the same
path is picked up by OpenSSH on its next connection; unchanged rehash does not
force reconnect. Update/re-add the entry or reload the module for immediate
rotation (reload also drops dynamic entries).

## Process lifecycle

InspIRCd ignores `SIGCHLD`, so the module deliberately never retains or signals a
helper PID. A private close-on-exec socket ties companion lifetime to ownership.
The companion restores normal child reaping, owns exactly one SSH child, and
polls for socket closure every 200 ms. Delete, replacement, unload and daemon
exit close the socket. The companion kills and reaps its SSH child before exiting;
this cannot accidentally signal a recycled PID. Linux parent-death signaling
also kills SSH if the companion crashes. Unload does not block the IRC loop
waiting for child exit. Kernel-uninterruptible processes can delay OS cleanup.

SSH failure retries use monotonic elapsed time and exponential backoff of
1, 2, 4, 8, 16, 32, then 64 seconds, capped indefinitely. A child surviving 60
seconds resets the failure count. Failure to launch or loss of the companion
uses the same bounded retry schedule on the daemon background tick. There is no
network work or blocking wait in the IRC command path. Start forks a tiny exec
stub which closes inherited descriptors before launching the companion; network,
authentication, keepalive and retry work is outside the daemon.

## Verification

`tests/test_reverse_tunnel.cpp` tests validation, merge rules, limits, SSH argv
and backoff without a daemon. `tests/reverse_tunnel.sh` also builds an isolated
integration image from `inspircd:testing`; `run_tests.sh` discovers it in CI.
It runs the real module and companion with a synthetic SSH executable under
`docker run --network none`. It verifies permission denial, config and dynamic
CRUD, redacted status, rehash atomicity, preservation of unchanged processes,
reconnect delays, deletion, unload/reload volatility and daemon-crash cleanup.
It does not use real credentials or contact SSH/IRC infrastructure. This tests the
supervision contract, not a real encrypted data path or gateway configuration.

```sh
docker build -t inspircd:testing .
sh tests/reverse_tunnel.sh
```
