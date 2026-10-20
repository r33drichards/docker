// Included inside Script. All snapshots own their values; no native objects escape.
static bool Admin(const v8::FunctionCallbackInfo<v8::Value>& args, bool active = false)
{
	auto* self = Self(args);
	if (self->authority != Authority::Admin || (active && (self->loading || !self->dispatching)))
	{
		Error(args.GetIsolate(), "Native operation requires admin authority and an active callback");
		return false;
	}
	return true;
}
static v8::Local<v8::Object> UserSnapshot(v8::Local<v8::Context> context, User* user)
{
	auto* isolate = context->GetIsolate();
	auto value = v8::Object::New(isolate);
	Put(context, value, "id", Display(isolate, user->uuid));
	Put(context, value, "nick", Display(isolate, user->nick));
	Put(context, value, "username", Display(isolate, user->GetDisplayedUser()));
	Put(context, value, "hostname", Display(isolate, user->GetDisplayedHost()));
	Put(context, value, "realname", Display(isolate, user->GetRealName()));
	Put(context, value, "local", v8::Boolean::New(isolate, !!IS_LOCAL(user)));
	Put(context, value, "oper", v8::Boolean::New(isolate, user->IsOper()));
	return value;
}
static void UserByNick(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	if (!Admin(args))
		return;
	std::string name;
	if (args.Length() != 1)
	{
		Error(args.GetIsolate(), "Expected one name");
		return;
	}
	if (!Text(args.GetIsolate(), args[0], name))
		return;
	auto* user = ServerInstance->Users.FindNick(name);
	if (!user || user->quitting)
	{
		args.GetReturnValue().SetNull();
		return;
	}
	args.GetReturnValue().Set(UserSnapshot(args.GetIsolate()->GetCurrentContext(), user));
}
static void ChannelByName(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	if (!Admin(args))
		return;
	std::string name;
	if (args.Length() != 1)
	{
		Error(args.GetIsolate(), "Expected one name");
		return;
	}
	if (!Text(args.GetIsolate(), args[0], name))
		return;
	auto* channel = ServerInstance->Channels.Find(name);
	if (!channel)
	{
		args.GetReturnValue().SetNull();
		return;
	}
	auto* isolate = args.GetIsolate();
	auto context = isolate->GetCurrentContext();
	auto value = v8::Object::New(isolate);
	Put(context, value, "name", Display(isolate, channel->name));
	Put(context, value, "topic", Display(isolate, channel->topic));
	Put(context, value, "modes", Display(isolate, channel->ChanModes(false)));
	Put(context, value, "created", v8::Number::New(isolate, channel->age));
	auto members = v8::Array::New(isolate);
	uint32_t index = 0;
	for (const auto& entry : channel->GetUsers())
	{
		if (index >= 4096)
			break;
		auto member = UserSnapshot(context, entry.first);
		Put(context, member, "rank", v8::Number::New(isolate, entry.second->GetRank()));
		Put(context, member, "prefix",
			Display(isolate, entry.second->GetPrefixChar() ? std::string(1, entry.second->GetPrefixChar()) : ""));
		if (!members->Set(context, index++, member).FromMaybe(false))
			return;
	}
	Put(context, value, "members", members);
	Put(context, value, "truncated", v8::Boolean::New(isolate, channel->GetUsers().size() > index));
	args.GetReturnValue().Set(value);
}
static void Channels(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	if (!Admin(args))
		return;
	if (args.Length())
	{
		Error(args.GetIsolate(), "Expected no arguments");
		return;
	}
	auto context = args.GetIsolate()->GetCurrentContext();
	auto array = v8::Array::New(args.GetIsolate());
	uint32_t index = 0;
	for (const auto& entry : ServerInstance->Channels.GetChans())
	{
		if (index >= 4096)
		{
			Error(args.GetIsolate(), "Channel query exceeds 4096 results");
			return;
		}
		if (!array->Set(context, index++, Display(args.GetIsolate(), entry.first)).FromMaybe(false))
			return;
	}
	args.GetReturnValue().Set(array);
}
static void NativeOperation(const v8::FunctionCallbackInfo<v8::Value>& args, const char* name, int arity)
{
	if (!Admin(args, true))
		return;
	auto* self = Self(args);
	if (args.Length() != arity || self->effects.size() >= 16)
	{
		Error(args.GetIsolate(), "Wrong arity or too many effects");
		return;
	}
	Effect effect{self->actor, {}, {}, self->authority, false};
	effect.operation = name;
	effect.owner = self->generation;
	for (int i = 0; i < arity; ++i)
	{
		std::string value;
		if (!Text(args.GetIsolate(), args[i], value))
			return;
		if (value.size() > 400 || !WireText(value))
		{
			Error(args.GetIsolate(), "Invalid native argument");
			return;
		}
		effect.params.push_back(std::move(value));
	}
	self->effects.push_back(std::move(effect));
	args.GetReturnValue().Set(true);
}
#define HOST_OP(method, name, arity)                                                                                   \
	static void method(const v8::FunctionCallbackInfo<v8::Value>& args)                                                \
	{                                                                                                                  \
		NativeOperation(args, name, arity);                                                                            \
	}
HOST_OP(Join, "join", 3)
HOST_OP(Part, "part", 3)
HOST_OP(Kick, "kick", 3)
HOST_OP(SetTopic, "setTopic", 2)
HOST_OP(SetModes, "setModes", 2)
HOST_OP(SetHost, "setHost", 2)
HOST_OP(SetUsername, "setUsername", 2)
HOST_OP(SetRealname, "setRealname", 2)
HOST_OP(Disconnect, "disconnect", 2)
HOST_OP(AddBan, "addBan", 4)
HOST_OP(RemoveBan, "removeBan", 2)
HOST_OP(LoadModule, "loadModule", 1)
HOST_OP(UnloadModule, "unloadModule", 1)
HOST_OP(Rehash, "rehash", 0)
HOST_OP(Shutdown, "shutdown", 0)
#undef HOST_OP
static void RegisterCommand(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	auto* self = Self(args);
	if (!Admin(args))
		return;
	std::string name;
	if (!self->loading || args.Length() != 5 || !args[1]->IsUint32() || !args[2]->IsUint32() || !args[3]->IsBoolean() ||
		!args[4]->IsFunction())
	{
		Error(args.GetIsolate(), "registerCommand(name,min,max,oper,callback) is initialization-only");
		return;
	}
	if (!Text(args.GetIsolate(), args[0], name))
		return;
	auto minimum = args[1].As<v8::Uint32>()->Value(), maximum = args[2].As<v8::Uint32>()->Value();
	if (name.empty() || name.size() > 32 ||
		name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos || maximum > 15 ||
		minimum > maximum || self->commands.size() >= 32 || self->commands.count(name))
	{
		Error(args.GetIsolate(), "Invalid or duplicate command registration");
		return;
	}
	self->commands.emplace(name,
		CommandSpec{minimum, maximum, args[3]->BooleanValue(args.GetIsolate()),
			v8::Global<v8::Function>(args.GetIsolate(), args[4].As<v8::Function>())});
}
void PrepareTimers(v8::Isolate* isolate)
{
	if (loading || !timertransaction.expired())
		return;
	auto backup = std::make_shared<std::map<unsigned int, TimerSpec>>();
	for (const auto& pair : timers)
	{
		v8::Global<v8::Function> callback;
		if (!pair.second.callback.IsEmpty())
			callback.Reset(isolate, pair.second.callback.Get(isolate));
		backup->emplace(pair.first, TimerSpec{pair.second.due, pair.second.interval, std::move(callback)});
	}
	auto transaction = std::make_shared<LocalTransaction>();
	std::weak_ptr<js_webhooks::Generation> owner = generation;
	transaction->rollback = [this, owner, backup]()
	{
		auto alive = owner.lock();
		if (alive && alive->alive)
			timers = std::move(*backup);
	};
	Effect effect{actor, {}, {}, authority, false};
	effect.local = transaction;
	effects.push_back(std::move(effect));
	timertransaction = transaction;
}
static void SetTimer(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	if (!Admin(args))
		return;
	auto* self = Self(args);
	if (args.Length() != 3 || !args[0]->IsUint32() || !args[1]->IsBoolean() || !args[2]->IsFunction())
	{
		Error(args.GetIsolate(), "setTimer(seconds,repeat,callback) requires bounded arguments");
		return;
	}
	auto seconds = args[0].As<v8::Uint32>()->Value();
	if (!seconds || seconds > 86400 || self->timers.size() >= 32 || self->nexttimer == UINT_MAX)
	{
		Error(args.GetIsolate(), "Invalid timer interval or timer limit");
		return;
	}
	if (!self->loading && !self->dispatching)
	{
		Error(args.GetIsolate(), "Timer mutation requires a callback");
		return;
	}
	if (self->effects.size() >= 16)
	{
		Error(args.GetIsolate(), "Too many effects");
		return;
	}
	self->PrepareTimers(args.GetIsolate());
	auto id = ++self->nexttimer;
	self->timers.emplace(id,
		TimerSpec{ServerInstance->Time() + seconds, args[1]->BooleanValue(args.GetIsolate()) ? seconds : 0,
			v8::Global<v8::Function>(args.GetIsolate(), args[2].As<v8::Function>())});
	args.GetReturnValue().Set(id);
}
static void ClearTimer(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	if (!Admin(args))
		return;
	if (args.Length() != 1 || !args[0]->IsUint32())
	{
		Error(args.GetIsolate(), "Expected timer ID");
		return;
	}
	auto* self = Self(args);
	if (!self->loading && !self->dispatching)
	{
		Error(args.GetIsolate(), "Timer mutation requires a callback");
		return;
	}
	if (self->effects.size() >= 16)
	{
		Error(args.GetIsolate(), "Too many effects");
		return;
	}
	self->PrepareTimers(args.GetIsolate());
	args.GetReturnValue().Set(self->timers.erase(args[0].As<v8::Uint32>()->Value()) != 0);
}
static void RegisterHost(v8::Isolate* isolate, v8::Local<v8::ObjectTemplate> api, v8::Local<v8::External> data)
{
#define BIND(name, method) api->Set(Key(isolate, name), v8::FunctionTemplate::New(isolate, Safe<method>, data))
	BIND("registerCommand", RegisterCommand);

	BIND("setTimer", SetTimer);

	BIND("clearTimer", ClearTimer);
	BIND("userByNick", UserByNick);
	BIND("channelByName", ChannelByName);

	BIND("channels", Channels);
	BIND("join", Join);
	BIND("part", Part);
	BIND("kick", Kick);
	BIND("setTopic", SetTopic);

	BIND("setModes", SetModes);
	BIND("setHost", SetHost);
	BIND("setUsername", SetUsername);

	BIND("setRealname", SetRealname);
	BIND("disconnect", Disconnect);
	BIND("addBan", AddBan);

	BIND("removeBan", RemoveBan);
	BIND("loadModule", LoadModule);
	BIND("unloadModule", UnloadModule);

	BIND("rehash", Rehash);
	BIND("shutdown", Shutdown);

#undef BIND
}
