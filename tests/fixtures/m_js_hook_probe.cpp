/* SPDX-License-Identifier: MIT */
// Test-only native driver. Never included in a normal image/module installation.
#include "inspircd.h"
#include "message.h"
#include "numeric.h"
#include "xline.h"

namespace
{
class ProbeDetails final : public MessageDetails
{
public:
	ProbeDetails(const std::string& value)
		: MessageDetails(MessageType::PRIVMSG, value, ClientProtocol::TagMap())
	{
	}
	bool IsCTCP(std::string_view&, std::string_view&) const override { return false; }
	bool IsCTCP(std::string_view&) const override { return false; }
	bool IsCTCP() const override { return false; }
};
class ProbeCommand final : public Command
{
	std::string report;
	void Record(const char* stage, ModResult result)
	{
		report.append(stage)
			.append(":")
			.append(result == MOD_RES_ALLOW	 ? "allow"
					: result == MOD_RES_DENY ? "deny"
											 : "pass")
			.append(" ");
	}

public:
	explicit ProbeCommand(Module* owner)
		: Command(owner, "JSHOOKPROBE", 0, 2)
	{
	}
	CmdResult Handle(User* source, const Params& input) override
	{
		auto* user = IS_LOCAL(source);
		auto* target = ServerInstance->Modules.Find("javascript");
		auto* channel = ServerInstance->Channels.Find("#jsprobe");
		auto* membership = channel ? channel->GetUser(user) : nullptr;
		if (!user || !target || !membership || ServerInstance->ports.empty())
			return CmdResult::FAILURE;
		const std::string selected = input.empty() ? "*" : input[0];
		if (selected == "result")
		{
			user->WriteNotice("probe-result:" + report);
			return CmdResult::SUCCESS;
		}
		report.clear();
		std::string text = "probe", command = "PROBE";
		Params parameters;
		parameters.push_back(text);
		auto* ping = ServerInstance->Parser.GetHandler("PING");
		CUList exemptions{user};
		User::NeighborList neighbors{membership};
		User::NeighborExceptions exceptions{{user, true}};
		MessageTarget message_target(channel, '@');
		ProbeDetails details(text);
		auto* mode = ServerInstance->Modes.FindMode('i', MODETYPE_CHANNEL);
		if (!mode)
			return CmdResult::FAILURE;
		Modes::Change change(mode, true, text);
		Modes::ChangeList modes;
		modes.push(change);
		ModeParser::ModeProcessFlag processflags = ModeParser::MODE_NONE;
		ExtensionType extype = ExtensionType::USER;
		bool flag = false;
		char snomask = 'a';
		time_t timestamp = ServerInstance->Time();
		ModeHandler::Rank rank = 100;
		int descriptor = -1;
		KLine line(ServerInstance->Time(), 60, "probe", "probe", "probe", "example.invalid");
		auto* listener = ServerInstance->ports.front();
		const auto& address = listener->bind_sa;
		Numeric::Numeric numeric(999);
		numeric.push(text);
		ClientProtocol::Message message("NOTICE", user);
		message.PushParam(text);
		const auto oper = user->oper;
		const auto klass = user->GetClass();
		std::optional<Numeric::Numeric> errnum(numeric);
#include "module_hook_calls.inc"
		return CmdResult::SUCCESS;
	}
};
class ModuleHookProbe final : public Module
{
	ProbeCommand command;

public:
	ModuleHookProbe()
		: Module(VF_NONE, "Synthetic Module hook driver; test builds only")
		, command(this)
	{
	}
};
} // namespace
MODULE_INIT(ModuleHookProbe)
