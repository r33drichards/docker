/* SPDX-License-Identifier: MIT */
#pragma once
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace reverse_tunnel
{
struct Error : std::runtime_error { using std::runtime_error::runtime_error; };
inline void Require(bool ok, const char* message) { if (!ok) throw Error(message); }
inline bool Token(const std::string& s, size_t max = 32)
{
	return !s.empty() && s.size() <= max && s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos && s[0] != '-';
}
inline unsigned Port(const std::string& s)
{
	Require(!s.empty() && s.size() <= 5 && s.find_first_not_of("0123456789") == std::string::npos, "invalid port");
	unsigned n = static_cast<unsigned>(std::stoul(s));
	Require(n > 0 && n <= 65535, "port out of range");
	return n;
}
inline bool Host(const std::string& s)
{
	// Deliberately DNS/IPv4 only; no SSH options, IPv6 ambiguity, or wildcards.
	return !s.empty() && s.size() <= 253 && s.front() != '-' && s.front() != '.' && s.back() != '.' &&
		s.find("..") == std::string::npos && s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") == std::string::npos;
}
inline bool Path(const std::string& s)
{
	// OpenSSH expands percent tokens and tildes in filenames; disallow both.
	return !s.empty() && s.size() <= 240 && s[0] == '/' && s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/_.-") == std::string::npos;
}
struct Profile
{
	std::string host, user, identity, knownhosts, destination;
	unsigned port = 22, listenmin = 1024, listenmax = 65535, destmin = 1, destmax = 65535;
	void Validate() const
	{
		Require(Host(host) && Host(destination) && Token(user), "invalid profile host or user");
		Require(Path(identity) && Path(knownhosts), "invalid credential file reference");
		Require(port > 0 && port <= 65535 && listenmin >= 1024 && listenmin <= listenmax && listenmax <= 65535 && destmin > 0 && destmin <= destmax && destmax <= 65535, "invalid profile port bounds");
	}
};
struct Tunnel
{
	std::string profile;
	unsigned listen = 0, destination = 0;
	bool dynamic = false;
};
using Profiles = std::map<std::string, Profile>;
using Tunnels = std::map<std::string, Tunnel>;
inline void Validate(const Profiles& profiles, const Tunnels& tunnels)
{
	Require(profiles.size() <= 32 && tunnels.size() <= 32, "maximum 32 profiles and tunnels");
	for (const auto& p : profiles) { Require(Token(p.first), "invalid profile name"); p.second.Validate(); }
	std::set<std::string> listeners;
	for (const auto& t : tunnels)
	{
		Require(Token(t.first), "invalid tunnel name");
		auto found = profiles.find(t.second.profile);
		Require(found != profiles.end(), "unknown profile");
		const auto& p = found->second;
		Require(t.second.listen >= p.listenmin && t.second.listen <= p.listenmax && t.second.destination >= p.destmin && t.second.destination <= p.destmax, "tunnel port outside profile bounds");
		Require(listeners.insert(p.host + ":" + std::to_string(p.port) + ":" + std::to_string(t.second.listen)).second, "duplicate remote listener");
	}
}
inline std::vector<std::string> Arguments(const Profile& p, const Tunnel& t)
{
	return {"/usr/bin/ssh", "-F", "/dev/null", "-N", "-T", "-n", "-oBatchMode=yes",
		"-oStrictHostKeyChecking=yes", "-oUserKnownHostsFile=" + p.knownhosts,
		"-oGlobalKnownHostsFile=/dev/null", "-oUpdateHostKeys=no", "-oVerifyHostKeyDNS=no",
		"-oIdentitiesOnly=yes", "-oIdentityAgent=none", "-oPasswordAuthentication=no",
		"-oKbdInteractiveAuthentication=no", "-oPreferredAuthentications=publickey",
		"-oExitOnForwardFailure=yes", "-oConnectTimeout=10", "-oConnectionAttempts=1",
		"-oServerAliveInterval=15", "-oServerAliveCountMax=3", "-oForwardAgent=no",
		"-oPermitLocalCommand=no", "-oControlMaster=no", "-oControlPath=none", "-oLogLevel=QUIET",
		"-i", p.identity, "-p", std::to_string(p.port), "-l", p.user,
		"-R", "127.0.0.1:" + std::to_string(t.listen) + ":" + p.destination + ":" + std::to_string(t.destination), p.host};
}
inline Tunnels Merge(const Tunnels& current, Tunnels configured)
{
	for (const auto& t : current)
		if (t.second.dynamic)
		{
			Require(!configured.count(t.first), "configuration conflicts with dynamic tunnel; delete it first");
			configured.insert(t);
		}
	return configured;
}
inline unsigned Backoff(unsigned failures) { return 1U << std::min(failures, 6U); }
} // namespace reverse_tunnel
