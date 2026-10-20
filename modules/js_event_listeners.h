/* SPDX-License-Identifier: MIT */
#pragma once

// Include after HookSnapshot. These listeners subscribe dynamically, so optional
// providers can load/unload without leaving a native pointer in a JS callback.
#include "modules/account.h"
#include "modules/away.h"
#include "modules/cap.h"
#include "modules/httpd.h"
#include "modules/names.h"
#include "modules/server.h"
#include "modules/stats.h"
#include "modules/who.h"
#include "modules/whois.h"

class JavaScriptEventListeners final : public Account::EventListener,
									   public Away::EventListener,
									   public Cap::EventListener,
									   public Names::EventListener,
									   public Stats::EventListener,
									   public Who::EventListener,
									   public Who::MatchEventListener,
									   public Who::VisibleEventListener,
									   public Whois::EventListener,
									   public Whois::LineEventListener,
									   public ServerProtocol::LinkEventListener,
									   public ServerProtocol::SyncEventListener,
									   public HTTPACLEventListener,
									   public HTTPRequestEventListener
{
public:
	using Builder = std::function<bool(HookSnapshot&, v8::Local<v8::Object>)>;
	using Dispatch = std::function<ModResult(const char*, const std::string&, const Builder&)>;

private:
	Dispatch dispatch;
	static std::string Actor(User* user) { return user && IS_LOCAL(user) ? user->uuid : std::string(); }
	static bool ServerFields(HookSnapshot& snapshot, v8::Local<v8::Object> event, const Server* server)
	{
		return snapshot.Field(event, "serverId", server->GetId()) &&
			snapshot.Field(event, "serverName", server->GetName()) &&
			snapshot.Field(event, "serverDescription", server->GetDesc());
	}
	static bool WhoFields(HookSnapshot& snapshot, v8::Local<v8::Object> event, const Who::Request& request)
	{
		return snapshot.Field(event, "matchText", request.matchtext) &&
			snapshot.Field(event, "matchChannel", request.matchchan) &&
			snapshot.Field(event, "fuzzyMatch", request.fuzzy_match) && snapshot.Field(event, "whox", request.whox) &&
			snapshot.Field(event, "queryType", request.whox_querytype);
	}
	void AwayChanged(const char* stage, User* user, const std::optional<AwayState>& previous)
	{
		dispatch(stage, Actor(user),
			[&](HookSnapshot& snapshot, v8::Local<v8::Object> event)
			{
				return snapshot.Field(event, "user", user) &&
					snapshot.Field(event, "previouslyAway", previous.has_value()) &&
					snapshot.Field(event, "previousMessage", previous ? previous->message : std::string()) &&
					snapshot.Field(event, "previousTime", previous ? previous->time : time_t(0)) &&
					snapshot.Field(event, "away", user->away.has_value()) &&
					snapshot.Field(event, "message", user->away ? user->away->message : std::string()) &&
					snapshot.Field(event, "time", user->away ? user->away->time : time_t(0));
			});
	}
	void HTTPEvent(const char* stage, HTTPRequest& request)
	{
		// Authentication headers, cookies, request body, query and fragment are
		// deliberately omitted. Observers cannot claim/answer the native socket.
		dispatch(stage, std::string(),
			[&](HookSnapshot& snapshot, v8::Local<v8::Object> event)
			{
				return snapshot.Field(event, "method", request.GetType()) &&
					snapshot.Field(event, "path", request.GetPath()) &&
					snapshot.Field(event, "address", request.GetIP());
			});
	}

public:
	JavaScriptEventListeners(Module* parent, Dispatch handler)
		: Account::EventListener(parent)
		, Away::EventListener(parent)
		, Cap::EventListener(parent)
		, Names::EventListener(parent)
		, Stats::EventListener(parent)
		, Who::EventListener(parent)
		, Who::MatchEventListener(parent)
		, Who::VisibleEventListener(parent)
		, Whois::EventListener(parent)
		, Whois::LineEventListener(parent)
		, ServerProtocol::LinkEventListener(parent)
		, ServerProtocol::SyncEventListener(parent)
		, HTTPACLEventListener(parent)
		, HTTPRequestEventListener(parent)
		, dispatch(std::move(handler))
	{
	}

	void OnAccountChange(User* user, const std::string& account) override
	{
		dispatch("accountChange", Actor(user),
			[&](HookSnapshot& snapshot, v8::Local<v8::Object> event)
			{
				return snapshot.Field(event, "user", user) && snapshot.Field(event, "account", account) &&
					snapshot.Field(event, "loggedIn", !account.empty());
			});
	}
	ModResult OnUserPreAway(LocalUser* user, std::string& message) override
	{
		return dispatch("userPreAway", Actor(user), [&](HookSnapshot& snapshot, v8::Local<v8::Object> event)
			{ return snapshot.Field(event, "user", user) && snapshot.Field(event, "message", message); });
	}
	ModResult OnUserPreBack(LocalUser* user) override
	{
		return dispatch("userPreBack", Actor(user),
			[&](HookSnapshot& snapshot, v8::Local<v8::Object> event) { return snapshot.Field(event, "user", user); });
	}
	void OnUserAway(User* user, const std::optional<AwayState>& previous) override
	{
		AwayChanged("userAway", user, previous);
	}
	void OnUserBack(User* user, const std::optional<AwayState>& previous) override
	{
		AwayChanged("userBack", user, previous);
	}
	void OnCapAddDel(Cap::Capability* cap, bool adding) override
	{
		dispatch("capAddDel", std::string(), [&](HookSnapshot& snapshot, v8::Local<v8::Object> event)
			{ return snapshot.Field(event, "name", cap->GetName()) && snapshot.Field(event, "adding", adding); });
	}
	void OnCapValueChange(Cap::Capability* cap) override
	{
		// Values may be user-specific. Do not call GetValue with a fabricated user.
		dispatch("capValueChange", std::string(), [&](HookSnapshot& snapshot, v8::Local<v8::Object> event)
			{ return snapshot.Field(event, "name", cap->GetName()); });
	}
	ModResult OnNamesListItem(
		LocalUser* issuer, Membership* membership, std::string& prefixes, std::string& nick) override
	{
		return dispatch("namesListItem", Actor(issuer),
			[&](HookSnapshot& snapshot, v8::Local<v8::Object> event)
			{
				return snapshot.Field(event, "issuer", issuer) && snapshot.Field(event, "membership", membership) &&
					snapshot.Field(event, "prefixes", prefixes) && snapshot.Field(event, "nick", nick);
			});
	}
	ModResult OnStats(Stats::Context& stats) override
	{
		return dispatch("stats", Actor(stats.GetSource()),
			[&](HookSnapshot& snapshot, v8::Local<v8::Object> event)
			{
				// Stats rows may contain operator-only details or configuration secrets.
				return snapshot.Field(event, "source", stats.GetSource()) &&
					snapshot.Field(event, "symbol", stats.GetSymbol());
			});
	}
	void OnWhois(Whois::Context& whois) override
	{
		dispatch("whois", Actor(whois.GetSource()),
			[&](HookSnapshot& snapshot, v8::Local<v8::Object> event)
			{
				return snapshot.Field(event, "source", whois.GetSource()) &&
					snapshot.Field(event, "target", whois.GetTarget());
			});
	}
	ModResult OnWhoisLine(Whois::Context& whois, Numeric::Numeric& numeric) override
	{
		return dispatch("whoisLine", Actor(whois.GetSource()),
			[&](HookSnapshot& snapshot, v8::Local<v8::Object> event)
			{
				return snapshot.Field(event, "source", whois.GetSource()) &&
					snapshot.Field(event, "target", whois.GetTarget()) && snapshot.Field(event, "numeric", numeric);
			});
	}
	ModResult OnWhoLine(const Who::Request& request, LocalUser* source, User* user, Membership* membership,
		Numeric::Numeric& numeric) override
	{
		return dispatch("whoLine", Actor(source),
			[&](HookSnapshot& snapshot, v8::Local<v8::Object> event)
			{
				return WhoFields(snapshot, event, request) && snapshot.Field(event, "source", source) &&
					snapshot.Field(event, "user", user) && snapshot.Field(event, "membership", membership) &&
					snapshot.Field(event, "numeric", numeric);
			});
	}
	ModResult OnWhoMatch(const Who::Request& request, LocalUser* source, User* user) override
	{
		return dispatch("whoMatch", Actor(source),
			[&](HookSnapshot& snapshot, v8::Local<v8::Object> event)
			{
				return WhoFields(snapshot, event, request) && snapshot.Field(event, "source", source) &&
					snapshot.Field(event, "user", user);
			});
	}
	ModResult OnWhoVisible(const Who::Request& request, LocalUser* source, Membership* membership) override
	{
		return dispatch("whoVisible", Actor(source),
			[&](HookSnapshot& snapshot, v8::Local<v8::Object> event)
			{
				return WhoFields(snapshot, event, request) && snapshot.Field(event, "source", source) &&
					snapshot.Field(event, "membership", membership);
			});
	}
	void OnServerLink(const Server* server) override
	{
		dispatch("serverLink", std::string(),
			[&](HookSnapshot& snapshot, v8::Local<v8::Object> event) { return ServerFields(snapshot, event, server); });
	}
	void OnServerBurst(const Server* server) override
	{
		dispatch("serverBurst", std::string(),
			[&](HookSnapshot& snapshot, v8::Local<v8::Object> event) { return ServerFields(snapshot, event, server); });
	}
	void OnServerSplit(const Server* server, bool error) override
	{
		dispatch("serverSplit", std::string(), [&](HookSnapshot& snapshot, v8::Local<v8::Object> event)
			{ return ServerFields(snapshot, event, server) && snapshot.Field(event, "error", error); });
	}
	void OnSyncUser(User* user, Server& server) override
	{
		dispatch("syncUser", Actor(user), [&](HookSnapshot& snapshot, v8::Local<v8::Object> event)
			{ return ServerFields(snapshot, event, &server) && snapshot.Field(event, "user", user); });
	}
	void OnSyncChannel(Channel* channel, Server& server) override
	{
		dispatch("syncChannel", std::string(), [&](HookSnapshot& snapshot, v8::Local<v8::Object> event)
			{ return ServerFields(snapshot, event, &server) && snapshot.Field(event, "channel", channel); });
	}
	void OnSyncNetwork(Server& server) override
	{
		dispatch("syncNetwork", std::string(), [&](HookSnapshot& snapshot, v8::Local<v8::Object> event)
			{ return ServerFields(snapshot, event, &server); });
	}
	ModResult OnHTTPACLCheck(HTTPRequest& request) override
	{
		HTTPEvent("httpACLCheck", request);
		return MOD_RES_PASSTHRU;
	}
	ModResult OnHTTPRequest(HTTPRequest& request) override
	{
		HTTPEvent("httpRequest", request);
		return MOD_RES_PASSTHRU;
	}
};
