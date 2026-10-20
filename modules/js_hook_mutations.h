/* SPDX-License-Identifier: MIT */
#pragma once

// Explicit bounded patches. These objects own staged copies; Commit callbacks
// are run synchronously by DispatchHook only after the entire dispatch succeeds.
// Never expose a native pointer or let JavaScript supply a tag provider.
class HookPatchReader
{
	v8::Local<v8::Context> context;
	v8::Isolate* isolate;

public:
	explicit HookPatchReader(v8::Local<v8::Context> ctx)
		: context(ctx)
		, isolate(ctx->GetIsolate())
	{
	}
	bool Fail()
	{
		Error(isolate, "Invalid, unsupported, or unauthorized hook patch");
		return false;
	}
	template <typename F> bool Fields(v8::Local<v8::Value> value, std::initializer_list<const char*> allowed, F field)
	{
		if (!value->IsObject() || value->IsArray() || value->IsProxy())
			return Fail();
		auto object = value.As<v8::Object>();
		v8::Local<v8::Array> keys;
		if (!object->GetOwnPropertyNames(context, v8::ALL_PROPERTIES).ToLocal(&keys) || keys->Length() > allowed.size())
			return Fail();
		for (uint32_t i = 0; i < keys->Length(); ++i)
		{
			v8::Local<v8::Value> key, item;
			std::string name;
			if (!keys->Get(context, i).ToLocal(&key) || !Text(isolate, key, name))
				return false;
			bool known = false;
			for (const auto* candidate : allowed)
				known = known || name == candidate;
			if (!known)
				return Fail();
			if (!object->Get(context, key).ToLocal(&item) || !field(name, item))
				return false;
		}
		return true;
	}
	bool String(v8::Local<v8::Value> value, std::string& out, std::size_t limit = 400)
	{
		// Bound before UTF-8 conversion, then bound the actual wire bytes.
		if (!value->IsString() || value.As<v8::String>()->Length() > static_cast<int>(limit))
			return Fail();
		return Text(isolate, value, out) && (out.size() <= limit && WireText(out) ? true : Fail());
	}
	bool Boolean(v8::Local<v8::Value> value, bool& out)
	{
		if (!value->IsBoolean())
			return Fail();
		out = value->BooleanValue(isolate);
		return true;
	}
	template <typename F> bool Array(v8::Local<v8::Value> value, F item)
	{
		if (!value->IsArray() || value->IsProxy())
			return Fail();
		auto array = value.As<v8::Array>();
		if (array->Length() > 256)
			return Fail();
		for (uint32_t i = 0; i < array->Length(); ++i)
		{
			v8::Local<v8::Value> element;
			if (!array->Get(context, i).ToLocal(&element) || !item(element))
				return false;
		}
		return true;
	}
	bool Users(v8::Local<v8::Value> value, CUList& users, Authority authority)
	{
		CUList next = authority == Authority::Admin ? CUList() : users;
		if (!Array(value,
				[&](auto item)
				{
					std::string id;
					if (!String(item, id, 32))
						return false;
					auto* user = ServerInstance->Users.FindUUID(id, true);
					if (!user || user->quitting)
						return Fail();
					next.insert(user);
					return true;
				}))
			return false;
		users = std::move(next);
		return true;
	}
};

class HookMessageMutation
{
	MessageDetails& native;
	std::string text;
	CUList exemptions;
	ClientProtocol::TagMap tags;
	bool echo, echo_original, update_idle;

public:
	explicit HookMessageMutation(MessageDetails& details)
		: native(details)
		, text(details.text)
		, exemptions(details.exemptions)
		, tags(details.tags_out)
		, echo(details.echo)
		, echo_original(details.echo_original)
		, update_idle(details.update_idle)
	{
	}
	bool Apply(v8::Local<v8::Context> context, v8::Local<v8::Value> patch, Authority authority,
		std::vector<std::function<void()>>& commits)
	{
		HookPatchReader reader(context);
		if (!reader.Fields(patch, {"text", "exemptions", "tagsOut", "echo", "echoOriginal", "updateIdle"},
				[&](const std::string& name, auto value)
				{
					if (name == "text")
						return reader.String(value, text) && (!text.empty() || reader.Fail());
					if (name == "exemptions")
						return reader.Users(value, exemptions, authority);
					if (name == "echo")
						return reader.Boolean(value, echo);
					if (name == "echoOriginal")
						return reader.Boolean(value, echo_original);
					if (name == "updateIdle")
						return reader.Boolean(value, update_idle);
					ClientProtocol::TagMap next;
					std::size_t budget = 4096;
					if (!reader.Array(value,
							[&](auto item)
							{
								std::string tagname, tagvalue;
								bool hasname = false, hasvalue = false;
								if (!reader.Fields(item, {"name", "value"},
										[&](const std::string& key, auto entry)
										{
											if (key == "name")
											{
												hasname = true;
												return reader.String(entry, tagname, 128);
											}
											hasvalue = true;
											return reader.String(entry, tagvalue, 512);
										}))
									return false;
								auto existing = tags.find(tagname);
								// Native TagMap stores escaped wire values. Preserve an unchanged
								// snapshot value; escape every administrative replacement.
								const std::string wire_value =
									existing != tags.end() && tagvalue == existing->second.value
									? tagvalue
									: ClientProtocol::Message::EscapeTag(tagvalue);
								const std::size_t wire_size = tagname.size() + wire_value.size() + 2;
								if (!hasname || !hasvalue || existing == tags.end() || next.count(tagname) ||
									(authority != Authority::Admin && tagvalue != existing->second.value) ||
									wire_size > budget)
									return reader.Fail();
								budget -= wire_size;
								auto data = existing->second;
								data.value = wire_value;
								next.emplace(tagname, std::move(data));
								return true;
							}))
						return false;
					tags = std::move(next);
					return true;
				}))
			return false;
		commits.emplace_back(
			[this]
			{
				native.text = text;
				native.exemptions = exemptions;
				native.tags_out = tags;
				native.echo = echo;
				native.echo_original = echo_original;
				native.update_idle = update_idle;
			});
		return true;
	}
};

class HookPartMutation
{
	std::string& native_text;
	CUList& native_exemptions;
	std::string text;
	CUList exemptions;

public:
	HookPartMutation(std::string& reason, CUList& excepts)
		: native_text(reason)
		, native_exemptions(excepts)
		, text(reason)
		, exemptions(excepts)
	{
	}
	bool Apply(v8::Local<v8::Context> context, v8::Local<v8::Value> patch, Authority authority,
		std::vector<std::function<void()>>& commits)
	{
		HookPatchReader reader(context);
		if (!reader.Fields(patch, {"partmessage", "exemptions"},
				[&](const std::string& name, auto value)
				{
					return name == "partmessage" ? reader.String(value, text)
												 : reader.Users(value, exemptions, authority);
				}))
			return false;
		commits.emplace_back(
			[this]
			{
				native_text = text;
				native_exemptions = exemptions;
			});
		return true;
	}
};

class HookNeighborMutation
{
	User::NeighborList& native_include;
	User::NeighborExceptions& native_exceptions;
	User::NeighborList include;
	User::NeighborExceptions exceptions;

public:
	HookNeighborMutation(User::NeighborList& members, User::NeighborExceptions& excepts)
		: native_include(members)
		, native_exceptions(excepts)
		, include(members)
		, exceptions(excepts)
	{
	}
	bool Apply(v8::Local<v8::Context> context, v8::Local<v8::Value> patch, Authority authority,
		std::vector<std::function<void()>>& commits)
	{
		HookPatchReader reader(context);
		if (!reader.Fields(patch, {"includeChannels", "exceptions"},
				[&](const std::string& name, auto value)
				{
					if (name == "includeChannels")
					{
						User::NeighborList next;
						if (!reader.Array(value,
								[&](auto item)
								{
									std::string channel;
									if (!reader.String(item, channel, 64))
										return false;
									auto found = std::find_if(include.begin(), include.end(),
										[&](auto* member) { return member->chan->name == channel; });
									if (found == include.end() ||
										std::find(next.begin(), next.end(), *found) != next.end())
										return reader.Fail();
									next.push_back(*found);
									return true;
								}))
							return false;
						include = std::move(next);
						return true;
					}
					return reader.Array(value,
						[&](auto item)
						{
							std::string id;
							bool enabled = false, hasid = false, hasinclude = false;
							if (!reader.Fields(item, {"userId", "include"},
									[&](const std::string& key, auto entry)
									{
										if (key == "userId")
										{
											hasid = true;
											return reader.String(entry, id, 32);
										}
										hasinclude = true;
										return reader.Boolean(entry, enabled);
									}))
								return false;
							auto* user = ServerInstance->Users.FindUUID(id, true);
							if (!hasid || !hasinclude || !user || !IS_LOCAL(user) || user->quitting ||
								(enabled && authority != Authority::Admin))
								return reader.Fail();
							exceptions[user] = enabled;
							return true;
						});
				}))
			return false;
		commits.emplace_back(
			[this]
			{
				native_include = include;
				native_exceptions = exceptions;
			});
		return true;
	}
};

class HookJoinMutation
{
	std::string& native;
	std::string privileges;

public:
	explicit HookJoinMutation(std::string& privs)
		: native(privs)
		, privileges(privs)
	{
	}
	bool Apply(v8::Local<v8::Context> context, v8::Local<v8::Value> patch, Authority authority,
		std::vector<std::function<void()>>& commits)
	{
		HookPatchReader reader(context);
		if (authority != Authority::Admin)
			return reader.Fail();
		if (!reader.Fields(patch, {"privs"},
				[&](const std::string&, auto value)
				{
					if (!reader.String(value, privileges, 64))
						return false;
					std::string seen;
					for (unsigned char mode : privileges)
					{
						if (!ServerInstance->Modes.FindPrefixMode(mode) || seen.find(mode) != std::string::npos)
							return reader.Fail();
						seen.push_back(mode);
					}
					return true;
				}))
			return false;
		commits.emplace_back([this] { native = privileges; });
		return true;
	}
};

class HookConnectMutation
{
	std::optional<Numeric::Numeric>& native;
	std::optional<Numeric::Numeric> numeric;

public:
	explicit HookConnectMutation(std::optional<Numeric::Numeric>& errnum)
		: native(errnum)
		, numeric(errnum)
	{
	}
	bool Apply(v8::Local<v8::Context> context, v8::Local<v8::Value> patch, Authority authority,
		std::vector<std::function<void()>>& commits)
	{
		HookPatchReader reader(context);
		if (authority != Authority::Admin)
			return reader.Fail();
		if (!reader.Fields(patch, {"errnum"},
				[&](const std::string&, auto value)
				{
					if (value->IsNull())
					{
						numeric.reset();
						return true;
					}
					unsigned int code = 0;
					bool hascode = false, hasparams = false;
					std::vector<std::string> params;
					if (!reader.Fields(value, {"code", "params"},
							[&](const std::string& name, auto entry)
							{
								if (name == "code")
								{
									if (!entry->IsUint32())
										return reader.Fail();
									code = entry.template As<v8::Uint32>()->Value();
									hascode = true;
									return (code >= 400 && code <= 599) || reader.Fail();
								}
								hasparams = true;
								return reader.Array(entry,
									[&](auto parameter)
									{
										std::string text;
										if (!reader.String(parameter, text))
											return false;
										params.push_back(std::move(text));
										return params.size() <= 14 || reader.Fail();
									});
							}))
						return false;
					if (!hascode || !hasparams || params.empty() || !js_wire::Validate("NUMERIC", params))
						return reader.Fail();
					numeric.emplace(code);
					for (const auto& param : params)
						numeric->push(param);
					return true;
				}))
			return false;
		commits.emplace_back([this] { native = numeric; });
		return true;
	}
};
