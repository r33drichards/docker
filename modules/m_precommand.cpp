/*
 * Observational pre- and post-command hooks for InspIRCd 4.
 * SPDX-License-Identifier: MIT
 */

#include "inspircd.h"
#include "parsed_command.h"

class ModulePreCommand final : public Module
{
public:
	ModulePreCommand()
		: Module(VF_NONE, "Logs unredacted pre- and post-command snapshots for debugging.")
	{
	}

	ModResult OnPreCommand(
		std::string& command, CommandBase::Params& parameters, LocalUser* user, bool validated) override
	{
		// Core normally calls this hook before and after validation. Only log the
		// latter so a command produces one record and arbitrary unknown command
		// text is not copied into the log.
		if (validated)
		{
			// Copy the full parsed values; never retain references to parser buffers.
			// This snapshot exists only for this callback, with no queue or export.
			const ParsedCommand parsed{command, parameters};

			// Debug output includes full values, including credentials and private
			// messages. Repr escapes bytes to prevent log-line injection.
			ServerInstance->Logs.Normal(MODNAME, "stage=OnPreCommand {} validated=true", parsed.Repr());
		}
		return MOD_RES_PASSTHRU;
	}

	void OnPostCommand(
		Command* command, const CommandBase::Params& parameters, LocalUser* user, CmdResult result, bool loop) override
	{
		const ParsedCommand parsed{command->name, parameters};
		const char* resultname = "INVALID";
		if (result == CmdResult::SUCCESS)
			resultname = "SUCCESS";
		else if (result == CmdResult::FAILURE)
			resultname = "FAILURE";
		ServerInstance->Logs.Normal(
			MODNAME, "stage=OnPostCommand {} result={} loop={}", parsed.Repr(), resultname, loop ? "true" : "false");
	}
};

MODULE_INIT(ModulePreCommand)
