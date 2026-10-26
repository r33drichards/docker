/* SPDX-License-Identifier: MIT */
#include "inspircd.h"
#include "reverse_tunnel_process.h"
#include <chrono>

namespace rt = reverse_tunnel;
using TunnelClock = std::chrono::steady_clock;

class TunnelManager
{
	struct Runtime
	{
		std::vector<std::string> args;
		int socket = -1;
		unsigned failures = 0;
		std::string state = "pending";
		TunnelClock::time_point retry{}, started{};
		explicit Runtime(std::vector<std::string> a) : args(std::move(a)) { }
		~Runtime() { if (socket >= 0) close(socket); }
	};
	std::map<std::string, std::unique_ptr<Runtime>> running;

public:
	rt::Profiles profiles;
	rt::Tunnels tunnels;
	void Apply(rt::Profiles nextprofiles, rt::Tunnels nexttunnels)
	{
		rt::Validate(nextprofiles, nexttunnels);
		// Validate the entire candidate before stopping or replacing any process.
		for (auto it = running.begin(); it != running.end(); )
		{
			auto t = nexttunnels.find(it->first);
			if (t == nexttunnels.end() || it->second->args != rt::Arguments(nextprofiles.at(t->second.profile), t->second))
				it = running.erase(it);
			else ++it;
		}
		profiles.swap(nextprofiles);
		tunnels.swap(nexttunnels);
		for (const auto& t : tunnels)
			if (!running.count(t.first)) running.emplace(t.first, std::make_unique<Runtime>(rt::Arguments(profiles.at(t.second.profile), t.second)));
	}
	void Tick()
	{
		const auto now = TunnelClock::now();
		for (auto& entry : running)
		{
			auto& r = *entry.second;
			if (r.socket >= 0)
			{
				char buffer[64];
				const ssize_t count = read(r.socket, buffer, sizeof(buffer));
				if (count > 0)
				{
					for (ssize_t i = 0; i < count; ++i) r.state = buffer[i] == 'S' ? "ssh-running-unverified" : "backoff";
				}
				else if (!count || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
				{
					close(r.socket);
					r.socket = -1;
					if (now - r.started >= std::chrono::seconds(60)) r.failures = 0;
					r.retry = now + std::chrono::seconds(rt::Backoff(r.failures));
					r.failures = std::min(r.failures + 1, 6U);
					r.state = "companion-backoff";
				}
			}
			else if (now >= r.retry)
			{
				r.socket = rt::Start(r.args);
				r.started = now;
				r.state = r.socket < 0 ? "companion-backoff" : "starting";
				r.retry = now + std::chrono::seconds(rt::Backoff(r.failures));
				if (r.socket < 0) r.failures = std::min(r.failures + 1, 6U);
			}
		}
	}
	std::string Status(const std::string& name) const
	{
		const auto& t = tunnels.at(name);
		// Do not expose endpoints, usernames, file references, PIDs, or SSH output.
		return name + " owner=" + (t.dynamic ? "dynamic" : "config") + " state=" + running.at(name)->state;
	}
};

class CommandReverseTunnel final : public Command
{
	TunnelManager& manager;
public:
	CommandReverseTunnel(Module* owner, TunnelManager& m) : Command(owner, "RTUNNEL", 1, 5), manager(m)
	{
		access_needed = CmdAccess::OPERATOR;
		syntax = {"LIST", "SHOW <name>", "ADD|UPDATE <name> <profile> <listenport> <destinationport>", "DELETE <name>"};
	}
	CmdResult Handle(User* user, const Params& p) override
	{
		if (!IS_LOCAL(user) || !user->HasPrivPermission("servers/reverse-tunnel"))
		{
			user->WriteNotice("RTUNNEL requires a local operator with servers/reverse-tunnel privilege");
			return CmdResult::FAILURE;
		}
		try
		{
			if (p[0] == "LIST" && p.size() == 1)
			{
				for (const auto& t : manager.tunnels) user->WriteNotice(manager.Status(t.first));
				user->WriteNotice("RTUNNEL end of list");
				return CmdResult::SUCCESS;
			}
			rt::Require(p.size() >= 2 && rt::Token(p[1]), "invalid command or name");
			auto next = manager.tunnels;
			auto found = next.find(p[1]);
			if (p[0] == "SHOW" && p.size() == 2)
			{
				rt::Require(found != next.end(), "unknown tunnel");
				user->WriteNotice(manager.Status(p[1]));
				return CmdResult::SUCCESS;
			}
			if (p[0] == "ADD" && p.size() == 5)
				rt::Require(found == next.end(), "tunnel already exists");
			else
			{
				rt::Require((p[0] == "UPDATE" && p.size() == 5) || (p[0] == "DELETE" && p.size() == 2), "invalid command syntax");
				rt::Require(found != next.end(), "unknown tunnel");
				rt::Require(found->second.dynamic, "config-owned tunnel; edit configuration and rehash");
			}
			if (p[0] == "DELETE") next.erase(p[1]);
			else next[p[1]] = {p[2], rt::Port(p[3]), rt::Port(p[4]), true};
			manager.Apply(manager.profiles, std::move(next));
			user->WriteNotice("RTUNNEL change accepted (dynamic changes are memory-only)");
			return CmdResult::SUCCESS;
		}
		catch (const rt::Error& e)
		{
			user->WriteNotice(std::string("RTUNNEL: ") + e.what());
			return CmdResult::FAILURE;
		}
	}
};

class ModuleReverseTunnel final : public Module
{
	TunnelManager manager;
	CommandReverseTunnel command;
public:
	ModuleReverseTunnel() : Module(VF_NONE, "Manages opt-in reverse SSH tunnels via a supervised OpenSSH companion."), command(this, manager) { }
	void ReadConfig(ConfigStatus&) override
	{
		try
		{
			rt::Profiles profiles;
			rt::Tunnels tunnels;
			for (const auto& [_, tag] : ServerInstance->Config->ConfTags("tunnelprofile"))
			{
				rt::Profile p;
				p.host = tag->getString("host");
				p.user = tag->getString("user");
				p.identity = tag->getString("identityfile");
				p.knownhosts = tag->getString("knownhostsfile");
				p.destination = tag->getString("destination");
				p.port = rt::Port(tag->getString("port", "22"));
				// Bounds are mandatory: operators cannot select arbitrary ports.
				p.listenmin = rt::Port(tag->getString("listenmin"));
				p.listenmax = rt::Port(tag->getString("listenmax"));
				p.destmin = rt::Port(tag->getString("destmin"));
				p.destmax = rt::Port(tag->getString("destmax"));
				rt::Require(profiles.emplace(tag->getString("name"), p).second, "duplicate profile name");
				rt::Require(profiles.size() <= 32, "maximum 32 profiles");
			}
			for (const auto& [_, tag] : ServerInstance->Config->ConfTags("reversetunnel"))
			{
				rt::Tunnel t{tag->getString("profile"), rt::Port(tag->getString("listenport")), rt::Port(tag->getString("destinationport")), false};
				rt::Require(tunnels.emplace(tag->getString("name"), t).second, "duplicate tunnel name");
				rt::Require(tunnels.size() <= 32, "maximum 32 tunnels");
			}
			manager.Apply(std::move(profiles), rt::Merge(manager.tunnels, std::move(tunnels)));
		}
		catch (const rt::Error& e) { throw ModuleException(this, std::string("Reverse tunnel configuration rejected: ") + e.what()); }
	}
	void OnBackgroundTimer(time_t) override { manager.Tick(); }
};
MODULE_INIT(ModuleReverseTunnel)
