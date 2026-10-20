static void HttpRequest(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	auto* caller = Self(args);
	if (caller->authority != Authority::Admin || caller->loading || !caller->dispatching)
	{
		Error(args.GetIsolate(), "Fetch requires an active admin callback");
		return;
	}
	auto* self = Self(args);
	std::string method = "GET", url, body;
	if (args.Length() < 1 || args.Length() > 2 || self->effects.size() >= 16)
	{
		Error(args.GetIsolate(), "Expected fetch URL and optional options");
		return;
	}
	if (!Text(args.GetIsolate(), args[0], url))
		return;
	if (args.Length() == 2)
	{
		if (!args[1]->IsObject() || args[1]->IsArray() || args[1]->IsProxy())
		{
			Error(args.GetIsolate(), "Expected fetch options object");
			return;
		}
		auto context = args.GetIsolate()->GetCurrentContext();
		auto options = args[1].As<v8::Object>();
		v8::Local<v8::Array> keys;
		if (!options->GetOwnPropertyNames(context).ToLocal(&keys))
			return;
		for (uint32_t index = 0; index < keys->Length(); ++index)
		{
			v8::Local<v8::Value> key, value;
			std::string name;
			if (!keys->Get(context, index).ToLocal(&key) || !Text(args.GetIsolate(), key, name) ||
				!options->Get(context, key).ToLocal(&value))
				return;
			if (name == "method")
			{
				if (!Text(args.GetIsolate(), value, method))
					return;
			}
			else if (name == "body")
			{
				if (!Text(args.GetIsolate(), value, body))
					return;
			}
			else
			{
				Error(args.GetIsolate(), "Unknown fetch option");
				return;
			}
		}
	}
	if (!self->http)
		self->http = std::make_shared<js_async_http::Dispatcher>();
	auto id = self->http->Stage(self->httpgeneration, method, url, body);
	if (id.empty())
	{
		Error(args.GetIsolate(), "Invalid HTTP request or queue full");
		return;
	}
	auto ticket = std::make_shared<HttpTicket>();
	ticket->transport = self->http;
	ticket->generation = self->httpgeneration;
	ticket->id = id;
	Effect effect{self->actor, {}, {}, self->authority, false};
	effect.http = std::move(ticket);
	self->effects.push_back(std::move(effect));
	args.GetReturnValue().Set(Display(args.GetIsolate(), id));
}
static void TakeHttpResult(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	if (Self(args)->authority != Authority::Admin)
	{
		Error(args.GetIsolate(), "Fetch requires admin authority");
		return;
	}
	if (args.Length() != 1)
	{
		Error(args.GetIsolate(), "Expected one request ID");
		return;
	}
	std::string id;
	if (!Text(args.GetIsolate(), args[0], id))
		return;
	auto* self = Self(args);
	js_async_http::Result result;
	if (!self->http || !self->http->Take(self->httpgeneration, id, result))
	{
		args.GetReturnValue().SetNull();
		return;
	}
	auto context = args.GetIsolate()->GetCurrentContext();
	auto value = v8::Object::New(args.GetIsolate());
	Put(context, value, "ok", v8::Boolean::New(args.GetIsolate(), result.ok));
	Put(context, value, "status", v8::Number::New(args.GetIsolate(), result.status));
	Put(context, value, "bodyBytes", Bytes(args.GetIsolate(), result.body));
	Put(context, value, "body", Display(args.GetIsolate(), result.body));
	Put(context, value, "error", Display(args.GetIsolate(), result.error));
	args.GetReturnValue().Set(value);
}
static void CancelHttp(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	if (Self(args)->authority != Authority::Admin)
	{
		Error(args.GetIsolate(), "Fetch requires admin authority");
		return;
	}
	if (args.Length() != 1)
	{
		Error(args.GetIsolate(), "Expected one request ID");
		return;
	}
	std::string id;
	if (!Text(args.GetIsolate(), args[0], id))
		return;
	auto* self = Self(args);
	args.GetReturnValue().Set(self->http && self->http->Cancel(self->httpgeneration, id));
}
static void RegisterFetch(v8::Isolate* isolate, v8::Local<v8::ObjectTemplate> api, v8::Local<v8::External> data)
{
	api->Set(Key(isolate, "fetch"), v8::FunctionTemplate::New(isolate, Safe<HttpRequest>, data));
	api->Set(Key(isolate, "takeFetchResult"), v8::FunctionTemplate::New(isolate, Safe<TakeHttpResult>, data));
	api->Set(Key(isolate, "cancelFetch"), v8::FunctionTemplate::New(isolate, Safe<CancelHttp>, data));
}
