/*
 * A minimal local ECHO command for InspIRCd 4.
 * SPDX-License-Identifier: MIT
 */

#include "inspircd.h"

class CommandEcho final
	: public SplitCommand
{
public:
	CommandEcho(Module* Creator)
		: SplitCommand(Creator, "ECHO", 1, 1)
	{
		syntax = { "<text>" };
	}

	CmdResult HandleLocal(LocalUser* user, const Params& parameters) override
	{
		// The command parser folds extra parameters into the single text argument.
		user->WriteNotice(parameters[0]);
		return CmdResult::SUCCESS;
	}
};

class ModuleEcho final
	: public Module
{
private:
	CommandEcho command;

public:
	ModuleEcho()
		: Module(VF_NONE, "Adds an ECHO command that returns text to the issuing client.")
		, command(this)
	{
	}
};

MODULE_INIT(ModuleEcho)
