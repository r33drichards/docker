/* SPDX-License-Identifier: MIT */
#include "../modules/reverse_tunnel.h"
#include <cassert>
#include <iostream>
using namespace reverse_tunnel;
template<class F> void Reject(F f) { bool rejected = false; try { f(); } catch (const Error&) { rejected = true; } assert(rejected); }
int main()
{
	Profile p{"gateway.example", "tunnel", "/run/secrets/key", "/run/secrets/hosts", "127.0.0.1", 22, 16667, 16670, 6667, 6697};
	Profiles profiles{{"edge", p}};
	Tunnels tunnels{{"irc", {"edge", 16667, 6667, false}}};
	Validate(profiles, tunnels);
	for (const auto& s : {"", "0", "65536", "-1", "22x", "1\n", "999999999999999999"}) Reject([&] { Port(s); });
	assert(Port("65535") == 65535);
	for (const auto& s : {"-oProxyCommand=x", "a:b", "host;id", "a b", "a\n", "a..b", "*"}) assert(!Host(s));
	for (const auto& s : {"relative", "/tmp/%h", "/tmp/a b", "~/.ssh/key", "/tmp/$(id)"}) assert(!Path(s));
	Reject([&] { auto t = tunnels; t["irc"].listen = 22; Validate(profiles, t); });
	Reject([&] { auto t = tunnels; t["irc"].destination = 80; Validate(profiles, t); });
	Reject([&] { auto t = tunnels; t["irc"].profile = "missing"; Validate(profiles, t); });
	Reject([&] { auto t = tunnels; t["duplicate"] = t.at("irc"); Validate(profiles, t); });
	Reject([&] { auto ps = profiles; ps["bad name"] = p; Validate(ps, tunnels); });
	Reject([&] { auto ps = profiles; ps["edge"].listenmin = 1; Validate(ps, tunnels); });
	Reject([&] { Tunnels t; for (int i = 0; i < 33; ++i) t[std::to_string(i)] = {"edge", 16667, 6667, true}; Validate(profiles, t); });
	auto dynamic = tunnels; dynamic["irc"].dynamic = true;
	assert(Merge(dynamic, {}).at("irc").dynamic);
	assert(Merge(tunnels, {}).empty());
	Reject([&] { Merge(dynamic, tunnels); });
	Reject([&] { Validate({}, Merge(dynamic, {})); });
	auto args = Arguments(p, tunnels.at("irc"));
	auto has = [&](const std::string& s) { return std::find(args.begin(), args.end(), s) != args.end(); };
	assert(args.front() == "/usr/bin/ssh" && args.back() == p.host);
	assert(has("-oStrictHostKeyChecking=yes") && has("-oIdentityAgent=none") && has("-oExitOnForwardFailure=yes"));
	assert(has("127.0.0.1:16667:127.0.0.1:6667") && has("-F") && has("/dev/null"));
	assert(Backoff(0) == 1 && Backoff(1) == 2 && Backoff(6) == 64 && Backoff(100) == 64);
	std::cout << "reverse tunnel validation, reconciliation, argv and backoff tests passed\n";
}
