/* SPDX-License-Identifier: MIT */
// Test-build only. Synthetic providers exercise only the javascript subscriber,
// never other modules: fake server/account events must not alter actual state.
#include "inspircd.h"
#include "modules/account.h"
#include "modules/cap.h"
#include "modules/httpd.h"
#include "modules/server.h"

namespace
{
class EventProbeCommand final : public Command
{
	Events::ModuleEventProvider account;
	Events::ModuleEventProvider cap;
	Events::ModuleEventProvider link;
	Events::ModuleEventProvider sync;
	Events::ModuleEventProvider httpacl;
	Events::ModuleEventProvider httprequest;
	Module* owner;

	template <typename Listener, typename Method, typename... Args>
	void CallTarget(Events::ModuleEventProvider& provider, Method method, Args&&... args)
	{
		auto* target = ServerInstance->Modules.Find("javascript");
		for (auto* subscriber : provider.GetSubscribers())
			if (subscriber->GetModule() == target)
				(static_cast<Listener*>(subscriber)->*method)(std::forward<Args>(args)...);
	}

public:
	explicit EventProbeCommand(Module* parent)
		: Command(parent, "JSEVENTPROBE", 0)
		, account(parent, "event/account")
		, cap(parent, "event/cap")
		, link(parent, "event/server-link")
		, sync(parent, "event/server-sync")
		, httpacl(parent, "event/http-acl")
		, httprequest(parent, "event/http-request")
		, owner(parent)
	{
	}
	CmdResult Handle(User* source, const Params&) override
	{
		auto* user = IS_LOCAL(source);
		auto* channel = ServerInstance->Channels.Find("#jsprobe");
		if (!user || !channel || !ServerInstance->Modules.Find("javascript"))
			return CmdResult::FAILURE;
		std::string accountname = "js-probe-account";
		CallTarget<Account::EventListener>(account, &Account::EventListener::OnAccountChange, source, accountname);
		accountname.clear();
		CallTarget<Account::EventListener>(account, &Account::EventListener::OnAccountChange, source, accountname);
		Cap::Capability capability(owner, "js-event-probe");
		CallTarget<Cap::EventListener>(cap, &Cap::EventListener::OnCapAddDel, &capability, true);
		CallTarget<Cap::EventListener>(cap, &Cap::EventListener::OnCapValueChange, &capability);
		CallTarget<Cap::EventListener>(cap, &Cap::EventListener::OnCapAddDel, &capability, false);
		Server server("999", "event-probe.example", "temporary probe server");
		CallTarget<ServerProtocol::LinkEventListener>(link, &ServerProtocol::LinkEventListener::OnServerLink, &server);
		CallTarget<ServerProtocol::LinkEventListener>(link, &ServerProtocol::LinkEventListener::OnServerBurst, &server);
		CallTarget<ServerProtocol::LinkEventListener>(
			link, &ServerProtocol::LinkEventListener::OnServerSplit, &server, true);
		CallTarget<ServerProtocol::SyncEventListener>(
			sync, &ServerProtocol::SyncEventListener::OnSyncUser, source, server);
		CallTarget<ServerProtocol::SyncEventListener>(
			sync, &ServerProtocol::SyncEventListener::OnSyncChannel, channel, server);
		CallTarget<ServerProtocol::SyncEventListener>(sync, &ServerProtocol::SyncEventListener::OnSyncNetwork, server);
		HTTPHeaders headers;
		headers.SetHeader("Authorization", "probe-auth-secret");
		headers.SetHeader("Cookie", "probe-cookie-secret");
		HTTPRequestURI uri;
		uri.path = "/js-event-probe";
		uri.query_params.emplace("token", "probe-query-secret");
		uri.fragment = "probe-fragment-secret";
		HTTPRequest request("GET", uri, &headers, nullptr, "127.0.0.1", "probe-body-secret");
		CallTarget<HTTPACLEventListener>(httpacl, &HTTPACLEventListener::OnHTTPACLCheck, request);
		CallTarget<HTTPRequestEventListener>(httprequest, &HTTPRequestEventListener::OnHTTPRequest, request);
		user->WriteNotice("event-probe:done");
		return CmdResult::SUCCESS;
	}
};
class ModuleEventProbe final : public Module
{
	EventProbeCommand command;

public:
	ModuleEventProbe()
		: Module(VF_NONE, "Synthetic event hook driver; test builds only")
		, command(this)
	{
	}
};
} // namespace
MODULE_INIT(ModuleEventProbe)
