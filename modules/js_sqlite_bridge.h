/* SPDX-License-Identifier: MIT */
// Included inside Script. Handles carry only host-owned state, never JS pointers.
static Database* DatabaseSelf(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	return static_cast<Database*>(args.Data().As<v8::External>()->Value());
}
static void DatabaseOpen(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	auto* self = Self(args);
	std::string name;
	if (args.Length() != 1 || !Text(args.GetIsolate(), args[0], name))
		return Error(args.GetIsolate(), "Expected one database name");
	if (!self->database_root || !self->databases.count(name))
		return Error(args.GetIsolate(), "SQLite database permission denied");
	if (self->database_handles.size() >= 16)
		return Error(args.GetIsolate(), "SQLite handle limit exceeded");
	auto handle = std::make_unique<Database>(self, name);
	auto data = v8::External::New(args.GetIsolate(), handle.get());
	auto object = v8::ObjectTemplate::New(args.GetIsolate());
	object->Set(
		Key(args.GetIsolate(), "query"), v8::FunctionTemplate::New(args.GetIsolate(), Safe<DatabaseQuery>, data));
	object->Set(Key(args.GetIsolate(), "exec"), v8::FunctionTemplate::New(args.GetIsolate(), Safe<DatabaseExec>, data));
	object->Set(Key(args.GetIsolate(), "transaction"),
		v8::FunctionTemplate::New(args.GetIsolate(), Safe<DatabaseTransaction>, data));
	object->Set(
		Key(args.GetIsolate(), "close"), v8::FunctionTemplate::New(args.GetIsolate(), Safe<DatabaseClose>, data));
	v8::Local<v8::Object> instance;
	if (!object->NewInstance(args.GetIsolate()->GetCurrentContext()).ToLocal(&instance))
		return;
	self->database_handles.push_back(std::move(handle));
	args.GetReturnValue().Set(instance);
}
static bool DatabaseParams(const v8::FunctionCallbackInfo<v8::Value>& args, std::vector<js_sqlite::Value>& values)
{
	auto isolate = args.GetIsolate();
	if (args.Length() == 1)
		return true;
	if (!args[1]->IsArray())
	{
		Error(isolate, "SQLite parameters must be an array");
		return false;
	}
	auto array = args[1].As<v8::Array>();
	const unsigned length = array->Length();
	if (length > 128)
	{
		Error(isolate, "SQLite parameter limit exceeded");
		return false;
	}
	std::size_t total = 0;
	for (unsigned i = 0; i < length; ++i)
	{
		v8::Local<v8::Value> value;
		if (!array->Get(isolate->GetCurrentContext(), i).ToLocal(&value))
			return false;
		if (value->IsNull())
			values.emplace_back(nullptr);
		else if (value->IsNumber())
		{
			double number = value.As<v8::Number>()->Value();
			if (!std::isfinite(number) || (std::trunc(number) == number && std::abs(number) > 9007199254740991.0))
			{
				Error(isolate, "SQLite numbers must be finite; use BigInt for wide integers");
				return false;
			}
			if (std::trunc(number) == number)
				values.emplace_back(static_cast<int64_t>(number));
			else
				values.emplace_back(number);
		}
		else if (value->IsBigInt())
		{
			bool lossless;
			auto number = value.As<v8::BigInt>()->Int64Value(&lossless);
			if (!lossless)
			{
				Error(isolate, "SQLite integer exceeds signed 64-bit range");
				return false;
			}
			values.emplace_back(number);
		}
		else if (value->IsString())
		{
			if (value.As<v8::String>()->Length() > 1024 * 1024)
			{
				Error(isolate, "SQLite text limit exceeded");
				return false;
			}
			std::string text;
			if (!Text(isolate, value, text))
				return false;
			total += text.size();
			values.emplace_back(std::move(text));
		}
		else if (value->IsUint8Array())
		{
			auto bytes = value.As<v8::Uint8Array>();
			if (bytes->ByteLength() > 1024 * 1024)
			{
				Error(isolate, "SQLite blob limit exceeded");
				return false;
			}
			js_sqlite::Blob blob(bytes->ByteLength());
			if (bytes->CopyContents(blob.data(), blob.size()) != blob.size())
				return false;
			total += blob.size();
			values.emplace_back(std::move(blob));
		}
		else
		{
			Error(isolate, "Unsupported SQLite parameter type");
			return false;
		}
		if (total > 1024 * 1024)
		{
			Error(isolate, "SQLite parameter byte limit exceeded");
			return false;
		}
	}
	return true;
}
static v8::Local<v8::Value> DatabaseValue(v8::Isolate* isolate, const js_sqlite::Value& value)
{
	if (std::holds_alternative<std::nullptr_t>(value))
		return v8::Null(isolate);
	if (auto number = std::get_if<int64_t>(&value))
	{
		if (*number >= -9007199254740991LL && *number <= 9007199254740991LL)
			return v8::Number::New(isolate, static_cast<double>(*number));
		return v8::BigInt::New(isolate, *number);
	}
	if (auto number = std::get_if<double>(&value))
		return v8::Number::New(isolate, *number);
	if (auto text = std::get_if<std::string>(&value))
		return Display(isolate, *text);
	const auto& blob = std::get<js_sqlite::Blob>(value);
	return Bytes(isolate, std::string(blob.begin(), blob.end()));
}
static void DatabaseExecute(const v8::FunctionCallbackInfo<v8::Value>& args, bool rows)
{
	auto* handle = DatabaseSelf(args);
	auto isolate = args.GetIsolate();
	if (args.Length() < 1 || args.Length() > 2 || !args[0]->IsString() || args[0].As<v8::String>()->Length() > 65536)
		return Error(isolate, "Expected SQL string and optional parameter array");
	std::string sql;
	std::vector<js_sqlite::Value> params;
	if (!Text(isolate, args[0], sql) || !DatabaseParams(args, params))
		return;
	if (isolate->IsExecutionTerminating())
		return;
	const auto result = handle->connection.Execute(sql, params, rows, handle->owner->loading || rows);
	auto context = isolate->GetCurrentContext();
	auto object = v8::Object::New(isolate);
	auto columns = v8::Array::New(isolate, result.columns.size());
	auto output = v8::Array::New(isolate, result.rows.size());
	for (unsigned i = 0; i < result.columns.size(); ++i)
		if (!columns->Set(context, i, Display(isolate, result.columns[i])).FromMaybe(false))
			return;
	for (unsigned i = 0; i < result.rows.size(); ++i)
	{
		auto row = v8::Array::New(isolate, result.rows[i].size());
		for (unsigned j = 0; j < result.rows[i].size(); ++j)
			if (!row->Set(context, j, DatabaseValue(isolate, result.rows[i][j])).FromMaybe(false))
				return;
		if (!output->Set(context, i, row).FromMaybe(false))
			return;
	}
	if (!Put(context, object, "columns", columns) || !Put(context, object, "rows", output) ||
		!Put(context, object, "changes", v8::BigInt::New(isolate, result.changes)) ||
		!Put(context, object, "lastInsertRowid", v8::BigInt::New(isolate, result.last_insert_rowid)))
		return;
	args.GetReturnValue().Set(object);
}
static void DatabaseQuery(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	DatabaseExecute(args, true);
}
static void DatabaseExec(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	DatabaseExecute(args, false);
}
static void DatabaseClose(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	if (args.Length())
		return Error(args.GetIsolate(), "close takes no arguments");
	auto* handle = DatabaseSelf(args);
	if (handle->connection.InTransaction())
		return Error(args.GetIsolate(), "Cannot close inside a transaction");
	handle->connection.Close();
}
static void DatabaseTransaction(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	auto* handle = DatabaseSelf(args);
	auto isolate = args.GetIsolate();
	if (args.Length() != 1 || !args[0]->IsFunction() || handle->owner->loading)
		return Error(isolate, "transaction requires a synchronous callback after initialization");
	handle->connection.Begin();
	try
	{
		v8::Local<v8::Value> result;
		if (!args[0]
				.As<v8::Function>()
				->Call(isolate->GetCurrentContext(), v8::Undefined(isolate), 0, nullptr)
				.ToLocal(&result))
		{
			handle->connection.Rollback();
			return;
		}
		if (result->IsPromise() || isolate->IsExecutionTerminating())
		{
			handle->connection.Rollback();
			if (!isolate->IsExecutionTerminating())
				Error(isolate, "SQLite transaction callbacks must be synchronous");
			return;
		}
		handle->connection.Commit();
		args.GetReturnValue().Set(result);
	}
	catch (...)
	{
		handle->connection.Rollback();
		throw;
	}
}
