/*
 * An observational pre-command hook for InspIRCd 4.
 * SPDX-License-Identifier: MIT
 */

#include "inspircd.h"
#include "parsed_command.h"

class ModulePreCommand final
	: public Module
{
public:
	ModulePreCommand()
		: Module(VF_NONE, "Logs an unredacted representation of validated parsed commands for debugging.")
	{
	}

	ModResult OnPreCommand(std::string& command, CommandBase::Params& parameters, LocalUser* user, bool validated) override
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
			ServerInstance->Logs.Normal(MODNAME, "{} validated=true", parsed.Repr());
		}
		return MOD_RES_PASSTHRU;
	}
};

MODULE_INIT(ModulePreCommand)
