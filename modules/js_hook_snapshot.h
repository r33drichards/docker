/* SPDX-License-Identifier: MIT */
#pragma once

// Included after the module's UTF-8/byte helpers. No native pointer is installed
// in V8; every value below is a copy, valid even after users/modules are culled.
class HookSnapshot
{
	v8::Local<v8::Context> context;
	v8::Isolate* isolate;
	bool ok = true;
	std::size_t remaining = 262144;
	static constexpr std::size_t MaxItems = 256;

	v8::Local<v8::Value> Null() const { return v8::Null(isolate); }
	v8::Local<v8::Value> Object(std::initializer_list<std::pair<const char*, v8::Local<v8::Value>>> fields)
	{
		auto object = v8::Object::New(isolate);
		for (const auto& field : fields)
			Set(object, field.first, field.second);
		return object;
	}
	bool Set(v8::Local<v8::Object> object, const char* name, v8::Local<v8::Value> value)
	{
		ok = ok && !value.IsEmpty() && Put(context, object, name, value);
		return ok;
	}

public:
	explicit HookSnapshot(v8::Local<v8::Context> ctx)
		: context(ctx)
		, isolate(ctx->GetIsolate())
	{
	}
	bool Ok() const { return ok; }
	template <typename T> bool Field(v8::Local<v8::Object> object, const char* name, const T& value)
	{
		return Set(object, name, Value(value));
	}
	v8::Local<v8::Value> Value(const std::string& text)
	{
		const auto count = std::min({text.size(), remaining, std::size_t(65536)});
		remaining -= count;
		const auto copy = text.substr(0, count);
		return Object({{"text", Display(isolate, copy)}, {"bytes", Bytes(isolate, copy)},
			{"byteLength", v8::Number::New(isolate, static_cast<double>(text.size()))},
			{"truncated", v8::Boolean::New(isolate, count != text.size())}});
	}
	v8::Local<v8::Value> Value(char value) { return Value(value ? std::string(1, value) : std::string()); }
	v8::Local<v8::Value> Value(bool value) { return v8::Boolean::New(isolate, value); }
	template <typename T, std::enable_if_t<std::is_integral_v<T> || std::is_enum_v<T>, int> = 0>
	v8::Local<v8::Value> Value(T value)
	{
		const auto number = static_cast<long double>(value);
		if (number < -9007199254740991.0L || number > 9007199254740991.0L)
		{
			ok = false;
			return Null();
		}
		return v8::Number::New(isolate, static_cast<double>(value));
	}
	v8::Local<v8::Value> Value(User* user)
	{
		if (!user)
			return Null();
		return Object({{"id", Display(isolate, user->uuid)}, {"nick", Value(user->nick)},
			{"local", Value(IS_LOCAL(user) != nullptr)}, {"quitting", Value(bool(user->quitting))},
			{"address", Value(user->GetAddress())}});
	}
	v8::Local<v8::Value> Value(LocalUser* user) { return Value(static_cast<User*>(user)); }
	v8::Local<v8::Value> Value(Channel* channel) { return channel ? Object({{"name", Value(channel->name)}}) : Null(); }
	v8::Local<v8::Value> Value(Membership* membership)
	{
		return membership ? Object({{"user", Value(membership->user)}, {"channel", Value(membership->chan)},
								{"prefixes", Value(membership->GetAllPrefixModes())}})
						  : Null();
	}
	v8::Local<v8::Value> Value(Extensible* item)
	{
		if (!item)
			return Null();
		// InspIRCd deliberately disables RTTI; its immutable extension type tags
		// are the supported discriminator for these three native object types.
		if (item->extype == ExtensionType::USER)
			return Object({{"kind", Value(std::string("user"))}, {"value", Value(static_cast<User*>(item))}});
		if (item->extype == ExtensionType::CHANNEL)
			return Object({{"kind", Value(std::string("channel"))}, {"value", Value(static_cast<Channel*>(item))}});
		if (item->extype == ExtensionType::MEMBERSHIP)
			return Object(
				{{"kind", Value(std::string("membership"))}, {"value", Value(static_cast<Membership*>(item))}});
		return Null();
	}
	template <typename Range, typename Convert> v8::Local<v8::Value> List(const Range& values, Convert convert)
	{
		auto items = v8::Array::New(isolate);
		uint32_t index = 0;
		for (const auto& value : values)
		{
			if (index == MaxItems)
				break;
			auto copy = convert(value);
			ok = ok && !copy.IsEmpty() && items->Set(context, index++, copy).FromMaybe(false);
		}
		return Object({{"items", items}, {"length", v8::Number::New(isolate, static_cast<double>(values.size()))},
			{"truncated", Value(values.size() > MaxItems)}});
	}
	v8::Local<v8::Value> Value(const CUList& users)
	{
		return List(users, [&](User* user) { return Value(user); });
	}
	v8::Local<v8::Value> Value(const User::NeighborList& memberships)
	{
		return List(memberships, [&](Membership* membership) { return Value(membership); });
	}
	v8::Local<v8::Value> Value(const User::NeighborExceptions& exceptions)
	{
		return List(exceptions, [&](const auto& entry)
			{ return Object({{"user", Value(entry.first)}, {"include", Value(entry.second)}}); });
	}
	v8::Local<v8::Value> Value(const CommandBase::Params& params)
	{
		return List(params, [&](const std::string& text) { return Value(text); });
	}
	v8::Local<v8::Value> Value(const ClientProtocol::TagMap& tags)
	{
		return List(tags, [&](const auto& entry)
			{ return Object({{"name", Value(entry.first)}, {"value", Value(entry.second.value)}}); });
	}
	v8::Local<v8::Value> Value(const MessageTarget& target)
	{
		return Object({{"type", Value(target.type)}, {"name", Value(target.GetName())},
			{"status", Value(target.status)}, {"originalStatus", Value(target.original_status)},
			{"user", target.type == MessageTarget::TYPE_USER ? Value(target.Get<User>()) : Null()},
			{"channel", target.type == MessageTarget::TYPE_CHANNEL ? Value(target.Get<Channel>()) : Null()}});
	}
	v8::Local<v8::Value> Value(const MessageDetails& details)
	{
		return Object({{"type", Value(details.type)}, {"text", Value(details.text)},
			{"originalText", Value(details.original_text)}, {"echo", Value(details.echo)},
			{"echoOriginal", Value(details.echo_original)}, {"updateIdle", Value(details.update_idle)},
			{"exemptions", Value(details.exemptions)}, {"tagsIn", Value(details.tags_in)},
			{"tagsOut", Value(details.tags_out)}});
	}
	v8::Local<v8::Value> Value(const Modes::Change& change)
	{
		return Object({{"adding", Value(change.adding)}, {"mode", Value(change.mh->GetModeChar())},
			{"name", Value(change.mh->name)}, {"parameter", Value(change.param)}});
	}
	v8::Local<v8::Value> Value(const Modes::ChangeList& changes)
	{
		return List(changes.getlist(), [&](const auto& change) { return Value(change); });
	}
	v8::Local<v8::Value> Value(XLine* line)
	{
		return line ? Object({{"type", Value(line->type)}, {"mask", Value(line->Displayable())},
						  {"source", Value(line->source)}, {"reason", Value(line->reason)},
						  {"setTime", Value(line->set_time)}, {"duration", Value(line->duration)},
						  {"expiry", Value(line->expiry)}, {"fromConfig", Value(line->from_config)}})
					: Null();
	}
	v8::Local<v8::Value> Value(Module* module)
	{
		return module ? Object({{"file", Value(module->ModuleFile)}, {"description", Value(module->description)},
							{"version", Value(module->version)}, {"dying", Value(module->dying)}})
					  : Null();
	}
	v8::Local<v8::Value> Value(const irc::sockets::sockaddrs& address)
	{
		return Object(
			{{"address", Value(address.addr())}, {"port", Value(address.port())}, {"family", Value(address.family())}});
	}
	v8::Local<v8::Value> Value(ListenSocket* listener)
	{
		return listener ? Object({{"address", Value(listener->bind_sa)}, {"protocol", Value(listener->bind_protocol)}})
						: Null();
	}
	v8::Local<v8::Value> Value(const Numeric::Numeric& numeric)
	{
		return Object({{"code", Value(numeric.GetNumeric())}, {"params", Value(numeric.GetParams())}});
	}
	v8::Local<v8::Value> Value(const std::optional<Numeric::Numeric>& numeric)
	{
		return numeric ? Value(*numeric) : Null();
	}
	v8::Local<v8::Value> Value(const ServiceProvider& service)
	{
		return Object({{"name", Value(service.name)}, {"type", Value(static_cast<int>(service.service))}});
	}
	v8::Local<v8::Value> Value(const ClientProtocol::Message& message)
	{
		return Object({{"command", Value(std::string(message.GetCommand()))},
			{"params",
				List(message.GetParams(),
					[&](const auto& parameter) { return Value(static_cast<const std::string&>(parameter)); })},
			{"source", message.GetSource() ? Value(*message.GetSource()) : Null()},
			{"tags", Value(message.GetTags())}});
	}
	v8::Local<v8::Value> Value(const std::shared_ptr<OperAccount>& oper)
	{
		return oper ? Object({{"name", Value(oper->GetName())}, {"type", Value(oper->GetType())}}) : Null();
	}
	v8::Local<v8::Value> Value(const std::shared_ptr<ConnectClass>& klass)
	{
		return klass ? Object({{"name", Value(klass->GetName())}}) : Null();
	}
};
