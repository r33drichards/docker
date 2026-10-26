/* SPDX-License-Identifier: MIT */
#include "inspircd.h"
#include "reverse_tunnel_process.h"
#include <chrono>

namespace rt = reverse_tunnel;
using TunnelClock = std::chrono::steady_clock;

static uint64_t TunnelNow()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(TunnelClock::now().time_since_epoch()).count());
}

class CommandReverseTunnel final : public Command
{
	rt::Controller& manager;
public:
	CommandReverseTunnel(Module* owner, rt::Controller& m) : Command(owner, "RTUNNEL", 1, 6), manager(m)
	{
		access_needed = CmdAccess::OPERATOR;
		syntax = {"LIST", "SHOW <name>", "ADD|UPDATE <name> <profile> <listenport> <destinationport> [enabled|disabled]", "ENABLE|DISABLE|DELETE <name>"};
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
				for (const auto& t : manager.tunnels) user->WriteNotice(manager.Status(t.first, TunnelNow()));
				user->WriteNotice("RTUNNEL end of list");
				return CmdResult::SUCCESS;
			}
			rt::Require(p.size() >= 2 && rt::Token(p[1]), "invalid command or name");
			auto next = manager.tunnels;
			auto found = next.find(p[1]);
			if (p[0] == "SHOW" && p.size() == 2)
			{
				rt::Require(found != next.end(), "unknown tunnel");
				user->WriteNotice(manager.Status(p[1], TunnelNow()));
				return CmdResult::SUCCESS;
			}
			if (p[0] == "ADD" && (p.size() == 5 || p.size() == 6))
				rt::Require(found == next.end(), "tunnel already exists");
			else
			{
				rt::Require((p[0] == "UPDATE" && (p.size() == 5 || p.size() == 6)) || ((p[0] == "DELETE" || p[0] == "ENABLE" || p[0] == "DISABLE") && p.size() == 2), "invalid command syntax");
				rt::Require(found != next.end(), "unknown tunnel");
				rt::Require(found->second.dynamic, "config-owned tunnel; edit configuration and rehash");
			}
			if (p[0] == "DELETE") next.erase(p[1]);
			else if (p[0] == "ENABLE" || p[0] == "DISABLE") found->second.enabled = p[0] == "ENABLE";
			else
			{
				const bool enabled = p.size() == 6 ? rt::Enabled(p[5]) : (found == next.end() || found->second.enabled);
				next[p[1]] = {p[2], rt::Port(p[3]), rt::Port(p[4]), true, enabled};
			}
			manager.Submit(manager.profiles, std::move(next));
			user->WriteNotice("RTUNNEL desired-state change accepted (reconciliation pending; dynamic changes are memory-only)");
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
	rt::ProcessTransport transport;
	rt::Controller manager;
	CommandReverseTunnel command;
public:
	ModuleReverseTunnel() : Module(VF_NONE, "Manages opt-in reverse SSH tunnels via a supervised OpenSSH companion."), manager(transport), command(this, manager) { }
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
				rt::Tunnel t{tag->getString("profile"), rt::Port(tag->getString("listenport")), rt::Port(tag->getString("destinationport")), false, rt::Enabled(tag->getString("state", "enabled"))};
				rt::Require(tunnels.emplace(tag->getString("name"), t).second, "duplicate tunnel name");
				rt::Require(tunnels.size() <= 32, "maximum 32 tunnels");
			}
			manager.Submit(std::move(profiles), rt::Merge(manager.tunnels, std::move(tunnels)));
		}
		catch (const rt::Error& e) { throw ModuleException(this, std::string("Reverse tunnel configuration rejected: ") + e.what()); }
	}
	void OnBackgroundTimer(time_t) override { manager.Reconcile(TunnelNow()); }
};
MODULE_INIT(ModuleReverseTunnel)
