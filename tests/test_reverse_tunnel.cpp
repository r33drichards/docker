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
	assert(Enabled("enabled") && !Enabled("disabled"));
	Reject([] { Enabled("yes"); });
	for (const auto& s : {"", "0", "65536", "-1", "22x", "1\n", "999999999999999999"}) Reject([&] { Port(s); });
	assert(Port("65535") == 65535);
	for (const auto& s : {"-oProxyCommand=x", "a:b", "host;id", "a b", "a\n", "a..b", "*"}) assert(!Host(s));
	assert(SSHHost("gateway.example") && SSHHost("192.0.2.1"));
	for (const auto& s : {"::1", "2603:1020:1601:8::136", "2001:DB8:0:0:0:0:0:1", "::ffff:192.0.2.1"})
	{
		assert(SSHHost(s));
		auto v6 = profiles;
		v6["edge"].host = s;
		Validate(v6, tunnels);
		auto v6args = Arguments(v6.at("edge"), tunnels.at("irc"));
		assert(v6args.back() == s && v6args.size() == Arguments(p, tunnels.at("irc")).size());
		assert(std::find(v6args.begin(), v6args.end(), "-oStrictHostKeyChecking=yes") != v6args.end());
	}
	for (const auto& s : {"[::1]", "[::1]:22", "fe80::1%eth0", "2001:db8::zz", "2001:::1", "user@::1", "-oProxyCommand=id", "::1;id", "::1 id", "::1\n", "::1/128", ":::"})
		assert(!SSHHost(s));
	assert(!SSHHost(std::string("::1\0suffix", 11)));
	assert(ListenerHost("2001:DB8:0:0:0:0:0:1") == ListenerHost("2001:db8::1"));
	Reject([&] {
		auto v6 = profiles;
		v6["edge"].host = "2001:db8::1";
		v6["alias"] = v6.at("edge");
		v6["alias"].host = "2001:DB8:0:0:0:0:0:1";
		auto t = tunnels;
		t["other"] = t.at("irc");
		t["other"].profile = "alias";
		Validate(v6, t);
	});
	Reject([&] { auto v6 = profiles; v6["edge"].destination = "::1"; Validate(v6, tunnels); });
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
