// Included inside ModuleJavaScript. Callbacks never retain borrowed daemon objects.
bool InvokeHost(Script& script, v8::Global<v8::Function>& callback, const std::string& actor,
	const std::vector<std::string>& params)
{
	if (!state->alive || !script.runtime.IsHealthy() || state->running || state->internal)
		return false;
	FlagGuard running(state->running);
	FlagGuard dispatching(script.dispatching);
	script.actor = actor;
	script.effects.clear();
	std::string error;
	bool ok = script.runtime.WithContext(
		[&](v8::Isolate* isolate, v8::Local<v8::Context> context)
		{
			auto event = v8::Object::New(isolate);
			auto arguments = v8::Array::New(isolate, params.size());
			for (uint32_t i = 0; i < params.size(); ++i)
				if (!arguments->Set(context, i, Display(isolate, params[i])).FromMaybe(false))
					return false;
			if (!Put(context, event, "userId",
					actor.empty() ? v8::Local<v8::Value>(v8::Null(isolate)) : Display(isolate, actor)) ||
				!Put(context, event, "params", arguments))
				return false;
			v8::Local<v8::Value> arg = event, result;
			return callback.Get(isolate)->Call(context, v8::Undefined(isolate), 1, &arg).ToLocal(&result) &&
				js_runtime::Runtime::RequireSynchronous(isolate, result);
		},
		error);
	script.actor.clear();
	if (!script.runtime.IsHealthy())
		script.InvalidateHttp();
	if (ok)
		Schedule(script.effects);
	script.effects.clear();
	return ok;
}
void TickTimers()
{
	if (!state->alive || !active || state->running || state->internal)
		return;
	for (auto& script : active->scripts)
	{
		std::vector<unsigned int> due;
		for (const auto& timer : script->timers)
			if (timer.second.due <= ServerInstance->Time())
				due.push_back(timer.first);
		for (auto id : due)
		{
			auto found = script->timers.find(id);
			if (found == script->timers.end())
				continue;
			// Move the handle out so a callback can cancel itself without invalidating the handle being called.
			auto callback = std::move(found->second.callback);
			auto interval = found->second.interval;
			if (interval)
				found->second.due = ServerInstance->Time() + interval;
			else
				script->timers.erase(found);
			InvokeHost(*script, callback, {}, {});
			found = script->timers.find(id);
			if (interval && found != script->timers.end())
				found->second.callback = std::move(callback);
		}
	}
}
void ValidateCommands(const ScriptSet& candidate)
{
	std::set<std::string> names;
	for (const auto& script : candidate.scripts)
		for (const auto& pair : script->commands)
		{
			if (!names.insert(pair.first).second)
				throw std::runtime_error("Duplicate JS command");
			auto* handler = ServerInstance->Parser.GetHandler(pair.first);
			if (handler &&
				std::none_of(registeredcommands.begin(), registeredcommands.end(),
					[handler](const auto& current) { return current.get() == handler; }))
				throw std::runtime_error("JS command conflicts with existing native command");
		}
}
void InstallCommands()
{
	for (auto& script : active->scripts)
		for (auto& pair : script->commands)
		{
			auto* target = script.get();
			auto name = pair.first;
			auto& spec = pair.second;
			auto command = std::make_unique<JavaScriptCommand>(this, name, spec.minimum, spec.maximum, spec.oper,
				[this, target, name](LocalUser* user, const CommandBase::Params& params)
				{
					return InvokeHost(*target, target->commands.at(name).callback, user->uuid, params)
						? CmdResult::SUCCESS
						: CmdResult::FAILURE;
				});
			ServerInstance->Modules.AddService(*command);
			registeredcommands.push_back(std::move(command));
		}
}
