/*
 * An observational pre-command hook for InspIRCd 4.
 * SPDX-License-Identifier: MIT
 */

#include "inspircd.h"

class ModulePreCommand final
	: public Module
{
public:
	ModulePreCommand()
		: Module(VF_NONE, "Logs the shape of validated parsed commands with parameter values redacted.")
	{
	}

	ModResult OnPreCommand(std::string& command, CommandBase::Params& parameters, LocalUser* user, bool validated) override
	{
		// Core normally calls this hook before and after validation. Only log the
		// latter so a command produces one record and arbitrary unknown command
		// text is not copied into the log.
		if (validated)
		{
			std::string params;
			for (size_t index = 0; index < parameters.size(); ++index)
			{
				if (index)
					params += ',';
				params += "\"<redacted>\"";
			}

			// Parameter values, tags, raw lines, and user identity are never logged.
			ServerInstance->Logs.Normal(MODNAME,
				"command={} params=[{}] parameter_count={} validated=true",
				command, params, parameters.size());
		}
		return MOD_RES_PASSTHRU;
	}
};

MODULE_INIT(ModulePreCommand)
