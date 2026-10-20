/* SPDX-License-Identifier: MIT */
/// $CompilerFlags: -isystem /usr/include/node -pthread find_compiler_flags("libcurl")
/// $LinkerFlags: -lnode -pthread -Wl,-z,nodelete find_linker_flags("libcurl")

#include "inspircd.h"
#include "js_runtime.h"
#include "js_webhooks.h"
#include "js_async_http.h"
#include "js_wire_validation.h"
#include "js_script_loader.h"
#include "js_native_utilities.h"
#include <cstring>
#include <type_traits>
#include "message.h"
#include "numeric.h"
#include "xline.h"
#include "js_generated/hook_specs.h"
#include "js_event_specs.h"

namespace
{
using Authority = js_scripts::Authority;
struct HttpTicket
{
	std::shared_ptr<js_async_http::Dispatcher> transport;
	std::shared_ptr<js_async_http::Generation> generation;
	std::string id;
	bool committed = false;
	~HttpTicket()
	{
		if (!committed)
			transport->Cancel(generation, id);
	}
};
struct LocalTransaction
{
	bool committed = false;
	std::function<void()> rollback;
	~LocalTransaction()
	{
		if (!committed && rollback)
			rollback();
	}
};
struct Effect
{
	std::string actor, target, text;
	Authority authority;
	bool rename;
	std::shared_ptr<js_webhooks::Request> webhook = nullptr;
	std::shared_ptr<HttpTicket> http;
	std::shared_ptr<LocalTransaction> local;
	std::string operation;
	std::vector<std::string> params;
	std::weak_ptr<js_webhooks::Generation> owner;
};
struct DispatchState
{
	bool alive = true;
	bool internal = false;
	bool running = false;
};
struct FlagGuard
{
	bool& flag;
	bool old;
	explicit FlagGuard(bool& value)
		: flag(value)
		, old(value)
	{
		flag = true;
	}
	~FlagGuard() { flag = old; }
};

v8::Local<v8::String> Key(v8::Isolate* isolate, const char* value)
{
	// All callers supply short fixed ASCII keys.
	return v8::String::NewFromUtf8(isolate, value).ToLocalChecked();
}
void Error(v8::Isolate* isolate, const char* value)
{
	isolate->ThrowException(v8::Exception::TypeError(Key(isolate, value)));
}
bool Text(v8::Isolate* isolate, v8::Local<v8::Value> value, std::string& result)
{
	if (!value->IsString())
	{
		Error(isolate, "Expected a string");
		return false;
	}
	v8::String::Value utf16(isolate, value);
	for (int i = 0; i < utf16.length(); ++i)
	{
		auto c = (*utf16)[i];
		if (c >= 0xd800 && c <= 0xdbff)
		{
			if (++i >= utf16.length() || (*utf16)[i] < 0xdc00 || (*utf16)[i] > 0xdfff)
			{
				Error(isolate, "Unpaired UTF-16 surrogate");
				return false;
			}
		}
		else if (c >= 0xdc00 && c <= 0xdfff)
		{
			Error(isolate, "Unpaired UTF-16 surrogate");
			return false;
		}
	}
	v8::String::Utf8Value bytes(isolate, value);
	if (!*bytes)
		return false;
	result.assign(*bytes, bytes.length());
	return true;
}
bool WireText(const std::string& text)
{
	return text.find_first_of(std::string("\0\r\n", 3)) == std::string::npos;
}
bool ValidUtf8(const std::string& text)
{
	std::size_t i = 0;
	while (i < text.size())
	{
		unsigned char c = text[i++];
		if (c < 0x80)
			continue;
		unsigned int count, point, minimum;
		if (c >= 0xc2 && c <= 0xdf)
		{
			count = 1;
			point = c & 31;
			minimum = 0x80;
		}
		else if (c >= 0xe0 && c <= 0xef)
		{
			count = 2;
			point = c & 15;
			minimum = 0x800;
		}
		else if (c >= 0xf0 && c <= 0xf4)
		{
			count = 3;
			point = c & 7;
			minimum = 0x10000;
		}
		else
			return false;
		if (text.size() - i < count)
			return false;
		while (count--)
		{
			unsigned char next = text[i++];
			if ((next & 0xc0) != 0x80)
				return false;
			point = (point << 6) | (next & 63);
		}
		if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff))
			return false;
	}
	return true;
}
v8::Local<v8::Uint8Array> Bytes(v8::Isolate* isolate, const std::string& bytes)
{
	auto buffer = v8::ArrayBuffer::New(isolate, bytes.size());
	if (!bytes.empty())
		std::memcpy(buffer->GetBackingStore()->Data(), bytes.data(), bytes.size());
	return v8::Uint8Array::New(buffer, 0, bytes.size());
}
bool Put(v8::Local<v8::Context> context, v8::Local<v8::Object> object, const char* name, v8::Local<v8::Value> value)
{
	return object->Set(context, Key(context->GetIsolate(), name), value).FromMaybe(false);
}
v8::Local<v8::Value> Display(v8::Isolate* isolate, const std::string& text)
{
	if (!ValidUtf8(text))
		return v8::Null(isolate);
	v8::Local<v8::String> value;
	if (!v8::String::NewFromUtf8(isolate, text.data(), v8::NewStringType::kNormal, text.size()).ToLocal(&value))
		return v8::Null(isolate);
	return value;
}

#include "js_hook_snapshot.h"
#include "js_hook_mutations.h"

const HookSpec* FindHook(const std::string& stage)
{
	for (const auto& hook : HookSpecs)
		if (stage == hook.name)
			return &hook;
	for (const auto& hook : EventHookSpecs)
		if (stage == hook.name)
			return &hook;
	return nullptr;
}

class Script
{
public:
	js_runtime::Runtime runtime;
	std::map<std::string, std::vector<v8::Global<v8::Function>>> callbacks;
	bool dispatching = false;
	std::vector<Effect> effects;
	Authority authority;
	std::string actor;
	bool loading = true;
	struct CommandSpec
	{
		unsigned int minimum, maximum;
		bool oper;
		v8::Global<v8::Function> callback;
	};
	struct TimerSpec
	{
		time_t due, interval;
		v8::Global<v8::Function> callback;
	};
	std::map<std::string, CommandSpec> commands;
	std::map<unsigned int, TimerSpec> timers;
	unsigned int nexttimer = 0;
	std::weak_ptr<LocalTransaction> timertransaction;
	std::shared_ptr<js_async_http::Dispatcher> http;
	std::shared_ptr<js_async_http::Generation> httpgeneration = std::make_shared<js_async_http::Generation>();
	void InvalidateHttp()
	{
		httpgeneration->active = false;
		if (http)
			http->Invalidate(httpgeneration);
	}
	~Script() { InvalidateHttp(); }
	js_webhooks::Dispatcher& delivery;
	std::shared_ptr<js_webhooks::Generation> generation;
	const std::vector<js_scripts::WasmAsset>& wasm;

	Script(unsigned int timeout, Authority auth, const std::vector<js_scripts::WasmAsset>& assets,
		js_webhooks::Dispatcher& transport, std::shared_ptr<js_webhooks::Generation> owner)
		: runtime(timeout)
		, authority(auth)
		, delivery(transport)
		, generation(std::move(owner))
		, wasm(assets)
	{
	}

	static Script* Self(const v8::FunctionCallbackInfo<v8::Value>& args)
	{
		return static_cast<Script*>(args.Data().As<v8::External>()->Value());
	}

	template <void (*Callback)(const v8::FunctionCallbackInfo<v8::Value>&)>
	static void Safe(const v8::FunctionCallbackInfo<v8::Value>& args)
	{
		try
		{
			Callback(args);
		}
		catch (...)
		{
			Error(args.GetIsolate(), "Native callback failed");
		}
	}
	static void On(const v8::FunctionCallbackInfo<v8::Value>& args)
	{
		auto* self = Self(args);
		std::string stage;
		if (!self->loading || args.Length() != 2 || !args[1]->IsFunction())
		{
			Error(args.GetIsolate(), "Register callbacks during initialization only");
			return;
		}
		if (!Text(args.GetIsolate(), args[0], stage))
			return;
		if (!FindHook(stage))
		{
			Error(args.GetIsolate(), "Unknown stage");
			return;
		}
		auto& callbacks = self->callbacks[stage];
		if (callbacks.size() >= 16)
		{
			Error(args.GetIsolate(), "Unknown stage or too many callbacks");
			return;
		}
		callbacks.emplace_back(args.GetIsolate(), args[1].As<v8::Function>());
	}
	static void Wasm(const v8::FunctionCallbackInfo<v8::Value>& args)
	{
		std::string name;
		if (args.Length() != 1)
		{
			Error(args.GetIsolate(), "Expected one asset name");
			return;
		}
		if (!Text(args.GetIsolate(), args[0], name))
			return;
		for (const auto& asset : Self(args)->wasm)
			if (asset.name == name)
			{
				args.GetReturnValue().Set(Bytes(args.GetIsolate(), asset.content));
				return;
			}
		Error(args.GetIsolate(), "Unknown configured Wasm asset");
	}
	static void PostWebhook(const v8::FunctionCallbackInfo<v8::Value>& args)
	{
		auto* self = Self(args);
		if (self->loading || !self->dispatching || self->authority != Authority::Admin)
		{
			Error(args.GetIsolate(), "Webhook posting requires an active admin-authority callback");
			return;
		}
		if (args.Length() != 2 || !args[0]->IsString() || !args[1]->IsString() ||
			args[0].As<v8::String>()->Length() > 64 ||
			static_cast<std::size_t>(args[1].As<v8::String>()->Length()) > js_webhooks::MaxBody)
		{
			Error(args.GetIsolate(), "Expected a bounded endpoint name and JSON object string");
			return;
		}
		std::string name, body;
		if (!Text(args.GetIsolate(), args[0], name) || !Text(args.GetIsolate(), args[1], body))
			return;
		if (!js_webhooks::ValidName(name) ||
			self->generation->endpoints.find(name) == self->generation->endpoints.end() ||
			body.size() > js_webhooks::MaxBody || self->effects.size() >= 16)
		{
			Error(args.GetIsolate(), "Unknown webhook destination or excessive webhook effect");
			return;
		}
		v8::Local<v8::Value> parsed;
		if (!v8::JSON::Parse(args.GetIsolate()->GetCurrentContext(), args[1].As<v8::String>()).ToLocal(&parsed))
			return;
		if (!parsed->IsObject() || parsed->IsArray())
		{
			Error(args.GetIsolate(), "Webhook JSON must contain an object");
			return;
		}
		auto request = self->delivery.Stage(self->generation, name, body, self->sourcepath, self->httpgeneration);
		const bool accepted = !!request;
		if (request)
		{
			Effect effect{self->actor, std::string(), std::string(), self->authority, false};
			effect.webhook = std::move(request);
			self->effects.push_back(std::move(effect));
		}
		args.GetReturnValue().Set(accepted);
	}
	static void WebhookStats(const v8::FunctionCallbackInfo<v8::Value>& args)
	{
		auto* self = Self(args);
		if (args.Length() != 0 || self->authority != Authority::Admin)
		{
			Error(args.GetIsolate(), "Webhook statistics require admin authority and no arguments");
			return;
		}
		const auto& stats = self->delivery.Stats();
		auto context = args.GetIsolate()->GetCurrentContext();
		auto value = v8::Object::New(args.GetIsolate());
		if (!Put(context, value, "pending", v8::Number::New(args.GetIsolate(), stats.pending.load())) ||
			!Put(context, value, "sent", v8::Number::New(args.GetIsolate(), stats.sent.load())) ||
			!Put(context, value, "failed", v8::Number::New(args.GetIsolate(), stats.failed.load())) ||
			!Put(context, value, "discarded", v8::Number::New(args.GetIsolate(), stats.discarded.load())))
			return;
		args.GetReturnValue().Set(value);
	}
	static void EffectCall(const v8::FunctionCallbackInfo<v8::Value>& args, bool rename)
	{
		auto* self = Self(args);
		std::string target, text;
		if (self->loading || !self->dispatching || (self->authority == Authority::User && self->actor.empty()))
		{
			Error(args.GetIsolate(), "State changes require an active callback and a local actor for user authority");
			return;
		}
		if (args.Length() != 2)
		{
			Error(args.GetIsolate(), "Expected target ID and text");
			return;
		}
		if (!Text(args.GetIsolate(), args[0], target) || !Text(args.GetIsolate(), args[1], text))
			return;
		if (self->effects.size() >= 16 || target.size() > 32 || !WireText(target) || !WireText(text) ||
			text.size() > 400)
		{
			Error(args.GetIsolate(), "Invalid or excessive state change");
			return;
		}
		if (self->authority == Authority::User && target != self->actor)
		{
			Error(args.GetIsolate(), "User authority can only target the triggering user");
			return;
		}
		if (rename && !ServerInstance->IsNick(text))
		{
			Error(args.GetIsolate(), "Invalid nickname");
			return;
		}
		self->effects.push_back({self->actor, target, text, self->authority, rename});
	}
	static void Notice(const v8::FunctionCallbackInfo<v8::Value>& args) { EffectCall(args, false); }
	static void Rename(const v8::FunctionCallbackInfo<v8::Value>& args) { EffectCall(args, true); }
#include "js_fetch_methods.inc"
#include "js_host_methods.inc"
	static void UserById(const v8::FunctionCallbackInfo<v8::Value>& args)
	{
		std::string uuid;
		if (args.Length() != 1)
		{
			Error(args.GetIsolate(), "Expected one user ID");
			return;
		}
		if (!Text(args.GetIsolate(), args[0], uuid))
			return;
		auto* user = ServerInstance->Users.FindUUID(uuid);
		if (!user || user->quitting)
		{
			args.GetReturnValue().SetNull();
			return;
		}
		auto context = args.GetIsolate()->GetCurrentContext();
		auto object = v8::Object::New(args.GetIsolate());
		if (!Put(context, object, "id", Display(args.GetIsolate(), user->uuid)) ||
			!Put(context, object, "nick", Display(args.GetIsolate(), user->nick)))
			return;
		args.GetReturnValue().Set(object);
	}
	std::string sourcepath;
	bool Load(const js_scripts::Source& source, std::string& error)
	{
		sourcepath = source.canonical_path.string();
		bool result = runtime.Initialize(
			source.content, source.canonical_path.string(),
			[this](v8::Isolate* isolate, v8::Local<v8::Context> context)
			{
				auto api = v8::ObjectTemplate::New(isolate);
				js_native::RegisterUtilities(isolate, api);
				auto data = v8::External::New(isolate, this);
				api->Set(Key(isolate, "on"), v8::FunctionTemplate::New(isolate, Safe<On>, data));
				api->Set(Key(isolate, "postWebhook"), v8::FunctionTemplate::New(isolate, Safe<PostWebhook>, data));
				api->Set(Key(isolate, "webhookStats"), v8::FunctionTemplate::New(isolate, Safe<WebhookStats>, data));
				api->Set(Key(isolate, "wasm"), v8::FunctionTemplate::New(isolate, Safe<Wasm>, data));
				api->Set(Key(isolate, "notice"), v8::FunctionTemplate::New(isolate, Safe<Notice>, data));
				api->Set(Key(isolate, "rename"), v8::FunctionTemplate::New(isolate, Safe<Rename>, data));
				api->Set(Key(isolate, "userById"), v8::FunctionTemplate::New(isolate, Safe<UserById>, data));
				RegisterFetch(isolate, api, data);
				RegisterHost(isolate, api, data);
				v8::Local<v8::Object> instance;
				return api->NewInstance(context).ToLocal(&instance) &&
					Put(context, context->Global(), "ircd", instance);
			},
			error);
		loading = false;
		return result;
	}
};

struct ScriptSet
{
	std::shared_ptr<js_webhooks::Generation> generation;
	std::vector<js_scripts::WasmAsset> wasm;
	std::vector<std::unique_ptr<Script>> scripts;
};

#include "js_host_effects.inc"

class DeferredEffect final : public ActionBase
{
	std::weak_ptr<DispatchState> state;
	Effect effect;

public:
	DeferredEffect(const std::shared_ptr<DispatchState>& owner, Effect value)
		: state(owner)
		, effect(std::move(value))
	{
	}
	void Call() override
	{
		ServerInstance->GlobalCulls.AddItem(this);
		auto owner = state.lock();
		if (!owner || !owner->alive)
			return;
		if (effect.webhook)
		{
			effect.webhook->Commit();
			return;
		}
		if (effect.http)
		{
			// Preserve callback effect ordering: accepted self-unload earlier in
			// this queue prevents this request from ever reaching a worker.
			effect.http->committed = effect.http->transport->Commit(effect.http->id);
			return;
		}
		if (!effect.operation.empty())
		{
			auto generation = effect.owner.lock();
			if (!generation || !generation->alive || effect.authority != Authority::Admin)
				return;
			FlagGuard guard(owner->internal);
			try
			{
				ApplyHostEffect(effect, *owner);
			}
			catch (...)
			{
				ServerInstance->Logs.Normal(MODNAME, "Deferred native operation failed");
			}
			return;
		}
		auto* actor = ServerInstance->Users.FindUUID<LocalUser>(effect.actor);
		auto* target = ServerInstance->Users.FindUUID<LocalUser>(effect.target);
		if (!target || target->quitting ||
			(effect.authority == Authority::User && (!actor || actor->quitting || target != actor)))
			return;
		FlagGuard guard(owner->internal);
		if (!effect.rename)
			target->WriteNotice(effect.text);
		else if (effect.authority == Authority::Admin)
		{
			if (ServerInstance->IsNick(effect.text))
				target->ChangeNick(effect.text);
		}
		else
		{
			std::string command = "NICK";
			CommandBase::Params parameters;
			parameters.push_back(effect.text);
			ServerInstance->Parser.ProcessCommand(actor, command, parameters);
		}
	}
};
} // namespace

#include "js_event_listeners.h"

class JavaScriptCommand final : public Command
{
	std::function<CmdResult(LocalUser*, const CommandBase::Params&)> invoke;

public:
	JavaScriptCommand(Module* module, const std::string& name, unsigned int minimum, unsigned int maximum, bool oper,
		std::function<CmdResult(LocalUser*, const CommandBase::Params&)> callback)
		: Command(module, name, minimum, maximum)
		, invoke(std::move(callback))
	{
		access_needed = oper ? CmdAccess::OPERATOR : CmdAccess::NORMAL;
	}
	CmdResult Handle(User* user, const Params& params) override
	{
		auto* local = IS_LOCAL(user);
		return local ? invoke(local, params) : CmdResult::FAILURE;
	}
};

class ModuleJavaScript final : public Module
{
	js_webhooks::Dispatcher delivery;
	std::unique_ptr<ScriptSet> active;
	std::shared_ptr<DispatchState> state = std::make_shared<DispatchState>();
	const CommandBase::Params* rewriteparams = nullptr;
	unsigned int dispatchdepth = 0;
	JavaScriptEventListeners events;
	std::vector<std::unique_ptr<JavaScriptCommand>> registeredcommands;
#include "js_host_dispatch.inc"

	static bool Snapshot(v8::Local<v8::Context> context, v8::Local<v8::Object>& event, const std::string& command,
		const std::vector<std::string>& parameters, const std::string& actor, const char* result, bool loop,
		bool validated)
	{
		auto* isolate = context->GetIsolate();
		event = v8::Object::New(isolate);
		auto params = v8::Array::New(isolate, parameters.size());
		auto raw = v8::Array::New(isolate, parameters.size());
		for (std::size_t i = 0; i < parameters.size(); ++i)
			if (!params->Set(context, i, Display(isolate, parameters[i])).FromMaybe(false) ||
				!raw->Set(context, i, Bytes(isolate, parameters[i])).FromMaybe(false))
				return false;
		return Put(context, event, "command", Display(isolate, command)) &&
			Put(context, event, "commandBytes", Bytes(isolate, command)) && Put(context, event, "params", params) &&
			Put(context, event, "paramBytes", raw) && Put(context, event, "userId", Display(isolate, actor)) &&
			Put(context, event, "stage", Key(isolate, result ? "postCommand" : "preCommand")) &&
			Put(context, event, "validated", v8::Boolean::New(isolate, validated)) &&
			Put(context, event, "result",
				result ? v8::Local<v8::Value>(Key(isolate, result)) : v8::Local<v8::Value>(v8::Null(isolate))) &&
			Put(context, event, "loop", v8::Boolean::New(isolate, loop));
	}
	static bool RewriteValue(v8::Local<v8::Context> context, v8::Local<v8::Value> value, std::string& command,
		std::vector<std::string>& params, bool& denied, bool validated)
	{
		auto* isolate = context->GetIsolate();
		if (value->IsUndefined())
			return true;
		if (!value->IsObject() || value->IsArray() || value->IsProxy())
		{
			Error(isolate, "Expected undefined or a decision object");
			return false;
		}
		auto decision = value.As<v8::Object>();
		v8::Local<v8::Array> keys;
		if (!decision->GetOwnPropertyNames(context).ToLocal(&keys))
			return false;
		for (uint32_t i = 0; i < keys->Length(); ++i)
		{
			v8::Local<v8::Value> key;
			std::string name;
			if (!keys->Get(context, i).ToLocal(&key) || !Text(isolate, key, name))
				return false;
			if (name != "action" && (validated || (name != "command" && name != "params")))
			{
				Error(isolate, "Unknown decision field or rewrite attempted after validation");
				return false;
			}
		}
		v8::Local<v8::Value> action, cmd, arguments;
		if (!decision->Get(context, Key(isolate, "action")).ToLocal(&action) ||
			!decision->Get(context, Key(isolate, "command")).ToLocal(&cmd) ||
			!decision->Get(context, Key(isolate, "params")).ToLocal(&arguments))
			return false;
		if (validated && (!cmd->IsUndefined() || !arguments->IsUndefined()))
		{
			Error(isolate, "Command rewrites are not permitted after validation");
			return false;
		}
		std::string actionname;
		if (!Text(isolate, action, actionname))
			return false;
		if (actionname == "deny")
		{
			denied = true;
			return true;
		}
		if (actionname != "pass")
		{
			Error(isolate, "Action must be pass or deny");
			return false;
		}
		if (cmd->IsUndefined() && arguments->IsUndefined())
			return true;
		if (!cmd->IsUndefined() && !Text(isolate, cmd, command))
			return false;
		if (!arguments->IsUndefined())
		{
			if (!arguments->IsArray())
			{
				Error(isolate, "params must be an array");
				return false;
			}
			auto array = arguments.As<v8::Array>();
			if (array->Length() > 15)
			{
				Error(isolate, "Too many parameters");
				return false;
			}
			params.clear();
			for (uint32_t i = 0; i < array->Length(); ++i)
			{
				v8::Local<v8::Value> parameter;
				if (!array->Get(context, i).ToLocal(&parameter))
					return false;
				std::string text;
				if (parameter->IsUint8Array())
				{
					auto bytes = parameter.As<v8::Uint8Array>();
					if (bytes->ByteLength() > 510)
					{
						Error(isolate, "Parameter too long");
						return false;
					}
					text.resize(bytes->ByteLength());
					if (bytes->CopyContents(text.data(), text.size()) != text.size())
						return false;
				}
				else if (!Text(isolate, parameter, text))
					return false;
				params.push_back(std::move(text));
			}
		}
		if (!js_wire::Validate(command, params))
		{
			Error(isolate, "Invalid IRC command rewrite or exceeded wire limits");
			return false;
		}
		return true;
	}
	bool Invoke(bool pre, std::string& command, std::vector<std::string>& params, const std::string& actor,
		const char* result, bool loop, bool& denied, std::vector<Effect>& effects, bool validated)
	{
		if (!active)
			return true;
		FlagGuard running(state->running);
		for (auto& script : active->scripts)
		{
			auto found = script->callbacks.find(pre ? "preCommand" : "postCommand");
			if (found == script->callbacks.end())
				continue;
			auto& callbacks = found->second;
			FlagGuard callbackflag(script->dispatching);
			script->actor = actor;
			script->effects.clear();
			std::string error;
			const bool washealthy = script->runtime.IsHealthy();
			bool ok = script->runtime.WithContext(
				[&](v8::Isolate* isolate, v8::Local<v8::Context> context)
				{
					for (auto& callback : callbacks)
					{
						v8::Local<v8::Object> event;
						if (!Snapshot(context, event, command, params, actor, result, loop, validated))
							return false;
						v8::Local<v8::Value> arg = event, returned;
						if (!callback.Get(isolate)->Call(context, v8::Undefined(isolate), 1, &arg).ToLocal(&returned) ||
							!js_runtime::Runtime::RequireSynchronous(isolate, returned))
							return false;
						if (pre && !RewriteValue(context, returned, command, params, denied, validated))
							return false;
						if (denied)
							break;
					}
					return true;
				},
				error);
			script->actor.clear();
			if (!ok || denied)
			{
				if (!script->runtime.IsHealthy())
					script->InvalidateHttp();
				script->effects.clear();
				effects.clear();
				if (!ok && washealthy)
					ServerInstance->Logs.Normal(
						MODNAME, "JavaScript callback failed; script disabled until successful rehash");
				return ok;
			}
			effects.insert(effects.end(), script->effects.begin(), script->effects.end());
			script->effects.clear();
		}
		return true;
	}
	void Schedule(std::vector<Effect>& effects)
	{
		for (auto& effect : effects)
		{
			if (effect.local)
				effect.local->committed = true;
			else
				ServerInstance->AtomicActions.AddAction(new DeferredEffect(state, std::move(effect)));
		}
	}

	static std::string Actor(User* user) { return user && IS_LOCAL(user) ? user->uuid : std::string(); }
	using MutationHandler = std::function<bool(
		v8::Local<v8::Context>, v8::Local<v8::Value>, Authority, std::vector<std::function<void()>>&)>;
	static bool HookDecision(v8::Local<v8::Context> context, v8::Local<v8::Value> returned, const HookSpec& hook,
		Authority authority, ModResult& result, const MutationHandler& mutate, const std::string& actor,
		std::vector<std::function<void()>>& commits)
	{
		auto* isolate = context->GetIsolate();
		if (returned->IsUndefined())
			return true;
		if (!returned->IsObject() || returned->IsArray() || returned->IsProxy())
		{
			Error(isolate, "Expected undefined or a hook decision");
			return false;
		}
		auto object = returned.As<v8::Object>();
		v8::Local<v8::Array> keys;
		if (!object->GetOwnPropertyNames(context, v8::ALL_PROPERTIES).ToLocal(&keys) || keys->Length() > 2)
			return false;
		for (uint32_t i = 0; i < keys->Length(); ++i)
		{
			v8::Local<v8::Value> key;
			std::string name;
			if (!keys->Get(context, i).ToLocal(&key) || !Text(isolate, key, name))
				return false;
			if (name != "action" && (name != "patch" || !mutate))
			{
				Error(isolate, "Unknown decision field or unsupported mutation");
				return false;
			}
		}
		v8::Local<v8::Value> action, patch;
		std::string name;
		if (!object->Get(context, Key(isolate, "action")).ToLocal(&action) || !Text(isolate, action, name) ||
			!object->Get(context, Key(isolate, "patch")).ToLocal(&patch))
			return false;
		if (name == "deny" && hook.decision)
			result = MOD_RES_DENY;
		else if (name == "allow" && hook.decision && hook.allow && authority == Authority::Admin)
			result = MOD_RES_ALLOW;
		else if (name != "pass")
		{
			Error(isolate, "Invalid action for this hook or script authority");
			return false;
		}
		if (!patch->IsUndefined())
		{
			if (name != "pass" || !mutate || (authority != Authority::Admin && actor.empty()))
			{
				Error(isolate, "Mutation requires action pass and a mutable hook");
				return false;
			}
			return mutate(context, patch, authority, commits);
		}
		return true;
	}

	using SnapshotBuilder = std::function<bool(HookSnapshot&, v8::Local<v8::Object>)>;
	ModResult DispatchHook(
		const char* stage, const std::string& actor, const SnapshotBuilder& build, const MutationHandler& mutate = {})
	{
		if (!state->alive || !active || state->internal || state->running)
			return MOD_RES_PASSTHRU;
		if (std::string(stage) == "backgroundTimer")
			TickTimers();
		const auto& hook = *FindHook(stage);
		FlagGuard running(state->running);
		std::vector<Effect> effects;
		std::vector<std::function<void()>> commits;
		ModResult result = MOD_RES_PASSTHRU;
		for (auto& script : active->scripts)
		{
			auto found = script->callbacks.find(stage);
			if (found == script->callbacks.end())
				continue;
			FlagGuard callbackflag(script->dispatching);
			script->actor = actor;
			script->effects.clear();
			std::string error;
			const bool washealthy = script->runtime.IsHealthy();
			const bool success = script->runtime.WithContext(
				[&](v8::Isolate* isolate, v8::Local<v8::Context> context)
				{
					for (auto& callback : found->second)
					{
						HookSnapshot snapshot(context);
						auto event = v8::Object::New(isolate);
						if (!Put(context, event, "stage", Key(isolate, stage)) ||
							!Put(context, event, "userId",
								actor.empty() ? v8::Local<v8::Value>(v8::Null(isolate)) : Display(isolate, actor)) ||
							!build(snapshot, event))
							return false;
						v8::Local<v8::Value> arg = event, returned;
						if (!callback.Get(isolate)->Call(context, v8::Undefined(isolate), 1, &arg).ToLocal(&returned) ||
							!js_runtime::Runtime::RequireSynchronous(isolate, returned))
							return false;
						if ((hook.decision || mutate) &&
							!HookDecision(context, returned, hook, script->authority, result, mutate, actor, commits))
							return false;
						if (result != MOD_RES_PASSTHRU)
							break;
					}
					return true;
				},
				error);
			script->actor.clear();
			if (!success || result == MOD_RES_DENY)
			{
				if (!script->runtime.IsHealthy())
					script->InvalidateHttp();
				script->effects.clear();
				if (!success && washealthy)
					ServerInstance->Logs.Normal(
						MODNAME, "JavaScript hook failed; script disabled until successful rehash");
				return success ? result : hook.decision && hook.failclosed ? MOD_RES_DENY : MOD_RES_PASSTHRU;
			}
			effects.insert(effects.end(), script->effects.begin(), script->effects.end());
			script->effects.clear();
			if (result != MOD_RES_PASSTHRU)
				break;
		}
		for (auto& commit : commits)
			commit();
		Schedule(effects);
		return result;
	}

public:
	ModuleJavaScript()
		: Module(VF_NONE, "Runs trusted local JavaScript and Wasm Module hooks with explicit authority.")
		, events(this, [this](const auto* stage, const auto& actor, const auto& build)
			  { return DispatchHook(stage, actor, build); })
	{
	}
	~ModuleJavaScript() override
	{
		state->alive = false;
		if (active)
			active->generation->alive = false;
		delivery.Stop();
	}
	void Prioritize() override
	{
		// Inspect/refuse accepted sockets before core_clients takes ownership.
		ServerInstance->Modules.SetPriority(this, I_OnAcceptConnection, PRIORITY_FIRST);
	}

#include "js_generated/hook_adapters.inc"
	void ReadConfig(ConfigStatus&) override
	{
		try
		{
			std::vector<js_scripts::SourceSpec> sources;
			std::vector<js_scripts::WasmSpec> assets;
			for (const auto& [_, tag] : ServerInstance->Config->ConfTags("javascript"))
			{
				auto file = tag->getString("file");
				auto directory = tag->getString("directory");
				auto permission = tag->getString("authority", "user");
				if (file.empty() == directory.empty() || (permission != "user" && permission != "admin"))
					throw std::runtime_error("javascript requires exactly one file/directory and user/admin authority");
				sources.push_back(
					{file.empty() ? js_scripts::SourceSpec::Kind::Directory : js_scripts::SourceSpec::Kind::File,
						file.empty() ? directory : file, permission == "admin" ? Authority::Admin : Authority::User});
			}
			for (const auto& [_, tag] : ServerInstance->Config->ConfTags("javascriptwasm"))
				assets.push_back({tag->getString("name"), tag->getString("file")});
			auto timeout =
				ServerInstance->Config->ConfValue("javascriptsettings")->getNum<unsigned int>("timeoutms", 25, 1, 1000);
			std::map<std::string, js_webhooks::Endpoint> endpoints;
			for (const auto& [_, tag] : ServerInstance->Config->ConfTags("javascriptwebhook"))
			{
				const auto name = tag->getString("name");
				if (!js_webhooks::ValidName(name) || endpoints.size() >= 16 || endpoints.count(name))
					throw std::runtime_error("Invalid, duplicate or excessive webhook endpoint");
				endpoints.emplace(
					name, js_webhooks::ValidateEndpoint(tag->getString("url"), tag->getBool("loopbackhttp", false)));
			}
			for (const auto& [_, tag] : ServerInstance->Config->ConfTags("javascriptwebhookheader"))
			{
				const auto found = endpoints.find(tag->getString("endpoint"));
				if (found == endpoints.end())
					throw std::runtime_error("Unknown webhook header endpoint");
				js_webhooks::AddHeader(found->second, tag->getString("name"), tag->getString("valuefile"));
			}
			auto loaded = js_scripts::LoadSources(sources, ServerInstance->Config->Paths.Config);
			js_webhooks::HeaderRules headers;
			for (const auto& [_, tag] : ServerInstance->Config->ConfTags("javascriptfetchheader"))
			{
				std::filesystem::path path = tag->getString("script");
				if (path.is_relative())
					path = std::filesystem::path(ServerInstance->Config->Paths.Config) / path;
				std::error_code ec;
				path = std::filesystem::canonical(path, ec);
				if (ec ||
					std::none_of(loaded.begin(), loaded.end(), [&](const auto& source)
						{ return source.canonical_path == path && source.authority == Authority::Admin; }))
					throw std::runtime_error("Fetch header requires a configured admin script");
				const auto url = tag->getString("url"), key = js_webhooks::ExactUrl(url);
				// Injection grants no additional network authority. HTTPS only, and
				// the ordinary fetch destination policy is checked independently.
				if (key.empty() || (key.compare(0, 8, "https://") != 0 && !js_async_http::FixtureUrl(url)) ||
					!js_async_http::ValidUrl(url))
					throw std::runtime_error("Invalid fetch header URL");
				const auto rule = std::make_pair(path.string(), key);
				if (!headers.count(rule) && headers.size() >= 32)
					throw std::runtime_error("Excessive fetch header rules");
				auto& endpoint = headers[rule];
				endpoint.url = url;
				js_webhooks::AddHeader(endpoint, tag->getString("name"), tag->getString("valuefile"));
			}
			// Ambiguous same-name legacy/rule credentials are config errors, not
			// an override order controlled by a script.
			for (const auto& [_, endpoint] : endpoints)
				for (const auto& [key, rule] : headers)
					if (js_webhooks::ExactUrl(endpoint.url) == key.second)
					{
						std::size_t bytes = 0;
						for (const auto* group : {endpoint.headers.get(), rule.headers.get()})
							for (const auto& [name, value] : *group)
								bytes += name.size() + value.size() + 4;
						if (bytes > js_webhooks::MaxHeaderBytes ||
							endpoint.headers->size() + rule.headers->size() > js_webhooks::MaxHeaders)
							throw std::runtime_error("Excessive combined HTTP headers");
						for (const auto& [name, value] : *rule.headers)
							if (endpoint.headers->count(name))
								throw std::runtime_error("Conflicting HTTP header rules");
					}
			auto candidate = std::make_unique<ScriptSet>();
			candidate->generation = std::make_shared<js_webhooks::Generation>(std::move(endpoints), std::move(headers));
			candidate->wasm = js_scripts::LoadWasmAssets(assets, ServerInstance->Config->Paths.Config);
			for (const auto& source : loaded)
			{
				auto script = std::make_unique<Script>(
					timeout, source.authority, candidate->wasm, delivery, candidate->generation);
				std::string error;
				if (!script->Load(source, error))
					throw std::runtime_error("JavaScript initialization failed (active scripts retained)");
				candidate->scripts.push_back(std::move(script));
			}
			ValidateCommands(*candidate);
			if (!candidate->generation->endpoints.empty())
				delivery.Start();
			if (active)
				active->generation->alive = false;
			candidate->generation->alive = true;
			registeredcommands.clear();
			active.swap(candidate);
			InstallCommands();
			ServerInstance->Logs.Normal(MODNAME, "JavaScript configuration loaded");
		}
		catch (const std::exception& error)
		{
			throw ModuleException(this, error.what());
		}
	}
	ModResult OnPreCommand(
		std::string& command, CommandBase::Params& parameters, LocalUser* user, bool validated) override
	{
		if (!state->alive || state->internal || state->running)
			return MOD_RES_PASSTHRU;
		if (dispatchdepth >= 8)
			return MOD_RES_DENY;
		if (!validated && &parameters == rewriteparams)
			return MOD_RES_PASSTHRU;
		std::string replacement = command;
		std::vector<std::string> params = parameters;
		std::vector<Effect> effects;
		bool denied = false;
		if (!Invoke(true, replacement, params, user->uuid, nullptr, false, denied, effects, validated) || denied)
			return MOD_RES_DENY;
		if (replacement != command || params != static_cast<const std::vector<std::string>&>(parameters))
		{
			// Core caches known handlers before this hook. Redispatch to resolve the
			// replacement handler and run ordinary validation under the same user.
			CommandBase::Params rewritten(params, parameters.GetTags());
			auto* target = ServerInstance->Parser.GetHandler(replacement);
			if (!target || target->access_needed == CmdAccess::SERVER)
				return MOD_RES_DENY;
			struct Restore
			{
				const CommandBase::Params*& slot;
				const CommandBase::Params* previous;
				~Restore() { slot = previous; }
			} restore{rewriteparams, rewriteparams};
			// Restore resets this slot before rewritten leaves scope on every exit.
			// cppcheck-suppress danglingLifetime
			rewriteparams = &rewritten;
			struct Depth
			{
				unsigned int& value;
				explicit Depth(unsigned int& count)
					: value(count)
				{
					++value;
				}
				~Depth() { --value; }
			} depth(dispatchdepth);
			Schedule(effects);
			ServerInstance->Parser.ProcessCommand(user, replacement, rewritten);
			return MOD_RES_DENY;
		}
		Schedule(effects);
		return MOD_RES_PASSTHRU;
	}
	void OnPostCommand(
		Command* handler, const CommandBase::Params& parameters, LocalUser* user, CmdResult result, bool loop) override
	{
		if (!state->alive || state->internal || state->running)
			return;
		std::string command = handler->name;
		std::vector<std::string> params = parameters;
		std::vector<Effect> effects;
		bool denied = false;
		const char* name = result == CmdResult::SUCCESS ? "SUCCESS"
			: result == CmdResult::FAILURE				? "FAILURE"
														: "INVALID";
		if (Invoke(false, command, params, user->uuid, name, loop, denied, effects, true))
			Schedule(effects);
	}
};
MODULE_INIT(ModuleJavaScript)
