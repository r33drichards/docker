// Runs only from the daemon atomic-action queue, after JS has returned.
void ApplyHostEffect(const Effect& effect, DispatchState& state)
{
	const auto& p = effect.params;
	const auto& op = effect.operation;
	auto* server = ServerInstance->FakeClient;
	if (op == "rehash")
	{
		ServerInstance->Rehash();
		return;
	}
	if (op == "shutdown")
	{
		ServerInstance->Exit(0);
		return;
	}
	if (op == "loadModule" || op == "unloadModule")
	{
		if (p[0].empty() ||
			p[0].find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-. ") !=
				std::string::npos ||
			p[0].find("..") != std::string::npos || p[0].find(' ') != std::string::npos)
			return;
		if (op == "loadModule")
			ServerInstance->Modules.Load(p[0]);
		else if (auto* module = ServerInstance->Modules.Find(p[0]))
		{
			const bool self = module == ServerInstance->Modules.Find(MODNAME);
			if (ServerInstance->Modules.Unload(module) && self)
			{
				// Unload itself is queued. Stop subsequent effects immediately rather
				// than waiting for our destructor at the next atomic-action turn.
				state.alive = false;
				if (auto generation = effect.owner.lock())
					generation->alive = false;
			}
		}
		return;
	}
	if (op == "addBan" || op == "removeBan")
	{
		auto* factory = ServerInstance->XLines->GetFactory(p[0]);
		if (!factory)
			return;
		if (op == "removeBan")
		{
			std::string reason;
			ServerInstance->XLines->DelLine(p[1], p[0], reason, server);
			return;
		}
		if (p[2].empty() || p[2].find_first_not_of("0123456789") != std::string::npos || p[2].size() > 9)
			return;
		auto duration = std::stoul(p[2]);
		std::unique_ptr<XLine> line(
			factory->Generate(ServerInstance->Time(), duration, ServerInstance->Config->ServerName, p[3], p[1]));
		if (line && ServerInstance->XLines->AddLine(line.get(), server))
		{
			// AddLine accepted ownership; releasing the guard must not delete it.
			(void)line.release();
			ServerInstance->XLines->ApplyLines();
		}
		return;
	}
	if (op == "setTopic" || op == "setModes")
	{
		auto* channel = ServerInstance->Channels.Find(p[0]);
		if (!channel)
			return;
		if (op == "setTopic")
			channel->SetTopic(server, p[1], ServerInstance->Time());
		else
		{
			std::vector<std::string> parameters{p[0]};
			irc::spacesepstream stream(p[1]);
			std::string word;
			while (stream.GetToken(word))
			{
				if (parameters.size() >= 16)
					return;
				parameters.push_back(word);
			}
			Modes::ChangeList changes;
			ServerInstance->Modes.ModeParamsToChangeList(server, MODETYPE_CHANNEL, parameters, changes);
			ServerInstance->Modes.Process(server, channel, nullptr, changes);
		}
		return;
	}
	auto* user = ServerInstance->Users.FindUUID(p[0]);
	if (!user || user->quitting)
		return;
	if (op == "setHost" && InspIRCd::IsHost(p[1], true))
		user->ChangeDisplayedHost(p[1]);
	else if (op == "setUsername" && ServerInstance->IsUser(p[1]))
		user->ChangeDisplayedUser(p[1]);
	else if (op == "setRealname")
		user->ChangeRealName(p[1]);
	else if (op == "disconnect")
		ServerInstance->Users.QuitUser(user, p[1]);
	else if (op == "join")
	{
		if (auto* local = IS_LOCAL(user))
			if (ServerInstance->Channels.IsChannel(p[1]))
				Channel::JoinUser(local, p[1], false, p[2]);
	}
	else if (op == "part" || op == "kick")
	{
		if (auto* channel = ServerInstance->Channels.Find(p[1]))
		{
			if (op == "part")
				channel->PartUser(user, p[2]);
			else
				channel->KickUser(server, user, p[2]);
		}
	}
}
