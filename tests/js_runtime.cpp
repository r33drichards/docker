/* SPDX-License-Identifier: MIT */

#include "../modules/js_runtime.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
void Check(bool condition, const std::string& message)
{
	if (!condition)
		throw std::runtime_error(message);
}

bool Evaluate(
	v8::Isolate* isolate, v8::Local<v8::Context> context, const std::string& source, v8::Local<v8::Value>& result)
{
	v8::Local<v8::String> text;
	v8::Local<v8::Script> script;
	return v8::String::NewFromUtf8(isolate, source.data(), v8::NewStringType::kNormal, static_cast<int>(source.size()))
			   .ToLocal(&text) &&
		v8::Script::Compile(context, text).ToLocal(&script) && script->Run(context).ToLocal(&result);
}

bool Invoke(js_runtime::Runtime& runtime, const std::string& expression, std::string& error)
{
	return runtime.WithContext(
		[&](v8::Isolate* isolate, v8::Local<v8::Context> context)
		{
			v8::Local<v8::Value> value;
			return Evaluate(isolate, context, expression, value) &&
				js_runtime::Runtime::RequireSynchronous(isolate, value);
		},
		error);
}

void AssertExpression(js_runtime::Runtime& runtime, const std::string& expression)
{
	std::string error;
	Check(runtime.WithContext(
			  [&](v8::Isolate* isolate, v8::Local<v8::Context> context)
			  {
				  v8::Local<v8::Value> value;
				  return Evaluate(isolate, context, expression, value) && value->IsTrue();
			  },
			  error),
		"expression failed: " + expression + "; " + error);
}

void CheckDisabled(js_runtime::Runtime& runtime)
{
	Check(!runtime.IsHealthy(), "failed runtime remained healthy");
	bool ran = false;
	std::string error;
	Check(!runtime.WithContext(
			  [&](v8::Isolate*, v8::Local<v8::Context>)
			  {
				  ran = true;
				  return true;
			  },
			  error),
		"poisoned runtime was reused");
	Check(!ran && error.find("disabled until reload") != std::string::npos,
		"disabled runtime ran work or did not explain recovery");
}

void TestInitializationAndIsolation()
{
	std::string error;
	js_runtime::Runtime first(1000);
	int calls = 0;
	Check(first.Initialize(
			  "globalThis.answer = nativeAdd(20, 22); void 0;", "first.js",
			  [&](v8::Isolate* isolate, v8::Local<v8::Context> context)
			  {
				  auto callback = [](const v8::FunctionCallbackInfo<v8::Value>& args)
				  {
					  auto* count = static_cast<int*>(args.Data().As<v8::External>()->Value());
					  ++*count;
					  args.GetReturnValue().Set(args[0].As<v8::Int32>()->Value() + args[1].As<v8::Int32>()->Value());
				  };
				  v8::Local<v8::Function> function;
				  return v8::Function::New(context, callback, v8::External::New(isolate, &calls)).ToLocal(&function) &&
					  context->Global()
						  ->Set(context, v8::String::NewFromUtf8Literal(isolate, "nativeAdd"), function)
						  .FromMaybe(false);
			  },
			  error),
		"host setup/evaluation failed: " + error);
	Check(calls == 1, "registered host callback was not called exactly once");
	AssertExpression(first, "answer === 42");
	AssertExpression(first,
		"typeof process === 'undefined' && typeof require === 'undefined'"
		" && typeof Buffer === 'undefined' && typeof setTimeout === 'undefined'"
		" && typeof queueMicrotask === 'undefined'");
	js_runtime::Runtime second(1000);
	Check(second.Initialize("void 0;", "second.js", {}, error), error);
	AssertExpression(second, "typeof answer === 'undefined' && typeof nativeAdd === 'undefined'");
	Check(first.GetIsolate() != second.GetIsolate(), "scripts shared an isolate");
	Check(first.GetIsolate()->GetMicrotasksPolicy() == v8::MicrotasksPolicy::kExplicit, "microtasks are not explicit");
}

void TestErrorHandling()
{
	std::string error;
	{
		js_runtime::Runtime runtime(1000);
		Check(!runtime.Initialize("function (", "syntax.js", {}, error), "invalid syntax was accepted");
		Check(error.find("syntax.js") != std::string::npos, "source filename missing from diagnostic");
		CheckDisabled(runtime);
	}
	{
		js_runtime::Runtime runtime(1000);
		Check(!runtime.Initialize("throw Error('PRIVATE_TOKEN');", "throw.js", {}, error),
			"throwing script was accepted");
		Check(error.find("PRIVATE_TOKEN") == std::string::npos, "exception content leaked");
		CheckDisabled(runtime);
	}
	{
		js_runtime::Runtime runtime(1000);
		Check(!runtime.Initialize("throw { toString() { while (true) {} } };", "hostile-error.js", {}, error),
			"throwing object was accepted");
		Check(error.find("exception") != std::string::npos, "throwing object was stringified");
	}
	{
		js_runtime::Runtime runtime(1000);
		Check(!runtime.Initialize(
				  "void 0;", "host-failure.js", [](v8::Isolate*, v8::Local<v8::Context>) -> bool
				  { throw std::runtime_error("PRIVATE_NATIVE_DETAIL"); }, error),
			"C++ setup exception escaped or was accepted");
		Check(error.find("PRIVATE_NATIVE_DETAIL") == std::string::npos, "native exception content leaked");
		CheckDisabled(runtime);
	}
	{
		js_runtime::Runtime runtime(1000);
		Check(runtime.Initialize("void 0;", "false.js", {}, error), error);
		Check(!runtime.WithContext([](v8::Isolate*, v8::Local<v8::Context>) { return false; }, error),
			"false host result was accepted");
		CheckDisabled(runtime);
	}
	{
		js_runtime::Runtime runtime(1000);
		Check(runtime.Initialize("void 0;", "twice.js", {}, error), error);
		Check(!runtime.Initialize("void 0;", "twice-again.js", {}, error), "reinitialization was accepted");
		CheckDisabled(runtime);
	}
}

void TestSynchronousOnly()
{
	std::string error;
	{
		js_runtime::Runtime runtime(1000);
		Check(runtime.Initialize("globalThis.observed = 0; Promise.resolve().then(() => { observed = 1; }); void 0;",
				  "microtasks.js", {}, error),
			error);
		AssertExpression(runtime, "observed === 0");
		AssertExpression(runtime, "observed === 0");
	}
	{
		js_runtime::Runtime runtime(1000);
		Check(runtime.Initialize("async function callback() { return true; }", "async-callback.js", {}, error), error);
		Check(!Invoke(runtime, "callback()", error), "async callback Promise was accepted");
		CheckDisabled(runtime);
	}
	{
		js_runtime::Runtime runtime(1000);
		Check(!runtime.Initialize("Promise.resolve(true)", "async-init.js", {}, error),
			"top-level Promise result was accepted");
		CheckDisabled(runtime);
	}
	{
		js_runtime::Runtime runtime(1000);
		Check(runtime.Initialize("void 0;", "atomics.js", {}, error), error);
		AssertExpression(runtime,
			"(() => { try { Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0);"
			" return false; } catch (error) { return error instanceof TypeError; } })()");
	}
}

void TestWebAssemblyErrors()
{
	std::string error;
	{
		js_runtime::Runtime runtime(1000);
		// Truncated wasm header: synchronous Module construction must fail
		// during initialization rather than publish a usable script runtime.
		Check(!runtime.Initialize(
				  "new WebAssembly.Module(new Uint8Array([0,97,115,109,1]));", "malformed-wasm.js", {}, error),
			"malformed wasm module was accepted");
		Check(error.find("JavaScript exception") != std::string::npos,
			"malformed wasm did not report a script exception: " + error);
		CheckDisabled(runtime);
	}
	{
		js_runtime::Runtime runtime(1000);
		// (module (func (export "trap") unreachable)): valid bytes compile
		// and instantiate; only calling the exported function should trap.
		const bool initialized =
			runtime.Initialize("const bytes = new Uint8Array([0,97,115,109,1,0,0,0,1,4,1,96,0,0,3,2,1,0,"
							   "7,8,1,4,116,114,97,112,0,0,10,5,1,3,0,0,11]);"
							   "const wasm = new WebAssembly.Instance(new WebAssembly.Module(bytes));"
							   "function callback() { wasm.exports.trap(); }",
				"trapping-wasm.js", {}, error);
		Check(initialized, "valid trapping wasm could not initialize: " + error);
		AssertExpression(runtime, "typeof wasm.exports.trap === 'function'");
		Check(!Invoke(runtime, "callback()", error), "wasm unreachable trap was accepted");
		Check(error.find("JavaScript exception") != std::string::npos,
			"wasm trap did not report a script exception: " + error);
		CheckDisabled(runtime);
	}
}

void TestTermination()
{
	std::string error;
	{
		js_runtime::Runtime runtime(30);
		const auto start = std::chrono::steady_clock::now();
		Check(!runtime.Initialize("while (true) {}", "infinite-init.js", {}, error), "infinite init survived");
		Check(error.find("timed out") != std::string::npos, "init did not time out: " + error);
		Check(std::chrono::steady_clock::now() - start < std::chrono::seconds(5), "init timeout was not bounded");
		CheckDisabled(runtime);
	}
	{
		js_runtime::Runtime runtime(30);
		Check(runtime.Initialize("function callback() { try { while (true) {} } catch (error) { return true; } }",
				  "infinite-callback.js", {}, error),
			error);
		Check(!Invoke(runtime, "callback()", error), "infinite callback caught termination");
		Check(error.find("timed out") != std::string::npos, "callback did not time out: " + error);
		CheckDisabled(runtime);
	}
	{
		js_runtime::Runtime runtime(30);
		Check(runtime.Initialize("const bytes = new Uint8Array([0,97,115,109,1,0,0,0,1,4,1,96,0,0,3,2,1,0,"
								 "7,8,1,4,115,112,105,110,0,0,10,9,1,7,0,3,64,12,0,11,11]);"
								 "const wasm = new WebAssembly.Instance(new WebAssembly.Module(bytes));"
								 "function callback() { wasm.exports.spin(); }",
				  "infinite-wasm.js", {}, error),
			error);
		const auto start = std::chrono::steady_clock::now();
		Check(!Invoke(runtime, "callback()", error), "infinite wasm callback survived");
		Check(error.find("timed out") != std::string::npos, "wasm did not time out: " + error);
		Check(std::chrono::steady_clock::now() - start < std::chrono::seconds(5), "wasm timeout was not bounded");
		CheckDisabled(runtime);
	}
	{
		js_runtime::Runtime runtime(1, 128);
		std::string source;
		source.reserve(360000);
		for (unsigned int index = 0; index < 120000; ++index)
			source += "1;\n";
		Check(!runtime.Initialize(source, "large-compilation.js", {}, error), "compilation escaped the time budget");
		Check(error.find("timed out") != std::string::npos, "compilation did not time out: " + error);
		CheckDisabled(runtime);
	}
	{
		js_runtime::Runtime runtime(10);
		Check(!runtime.Initialize(
				  "void 0;", "slow-setup.js",
				  [](v8::Isolate*, v8::Local<v8::Context>)
				  {
					  std::this_thread::sleep_for(std::chrono::milliseconds(25));
					  return true;
				  },
				  error),
			"host setup escaped the operation budget");
		Check(error.find("timed out") != std::string::npos, "setup did not time out: " + error);
	}
}

void TestLifetimeAndReentrancy()
{
	std::string error;
	for (unsigned int generation = 0; generation < 40; ++generation)
	{
		js_runtime::Runtime runtime(200);
		Check(runtime.Initialize("globalThis.value = 42;", "reload.js", {}, error), error);
		for (unsigned int call = 0; call < 20; ++call)
			AssertExpression(runtime, "value === 42");
	}
	{
		js_runtime::Runtime runtime(1000);
		Check(runtime.Initialize("void 0;", "owner-thread.js", {}, error), error);
		bool success = true;
		std::thread other(
			[&] { success = runtime.WithContext([](v8::Isolate*, v8::Local<v8::Context>) { return true; }, error); });
		other.join();
		Check(
			!success && error.find("non-owning thread") != std::string::npos, "cross-thread isolate use was accepted");
		AssertExpression(runtime, "true");
	}
	{
		js_runtime::Runtime runtime(1000);
		Check(runtime.Initialize("void 0;", "reentrant.js", {}, error), error);
		Check(!runtime.WithContext(
				  [&](v8::Isolate*, v8::Local<v8::Context>)
				  {
					  std::string inner_error;
					  Check(
						  !runtime.WithContext([](v8::Isolate*, v8::Local<v8::Context>) { return true; }, inner_error),
						  "reentrant isolate use was accepted");
					  return true;
				  },
				  error),
			"outer operation ignored reentrant failure");
		CheckDisabled(runtime);
	}
	// Destroy timed-out isolates immediately, then run fresh generations. Any
	// late watchdog would access disposed memory or poison the next operation.
	for (unsigned int generation = 0; generation < 15; ++generation)
	{
		{
			js_runtime::Runtime timed_out(2);
			Check(!timed_out.Initialize("for (;;) {}", "dispose-after-timeout.js", {}, error),
				"infinite script survived repeated timeout");
		}
		js_runtime::Runtime fresh(200);
		Check(fresh.Initialize("void 0;", "fresh-after-timeout.js", {}, error), error);
		AssertExpression(fresh, "6 * 7 === 42");
	}
}
} // namespace

int main()
{
	try
	{
		Check(std::string(js_runtime::Runtime::FailureCategory("JavaScript execution timed out (private/source.js)")) == "timeout", "Categorize timeout without source path");
		Check(std::string(js_runtime::Runtime::FailureCategory("JavaScript exception (https://private.example/?secret=token)")) == "exception", "Categorize exception without source URL");
		Check(std::string(js_runtime::Runtime::FailureCategory("password=secret")) == "unknown", "Do not echo arbitrary errors");
		Check(std::string(js_runtime::Runtime::FailureCategory("JavaScript exception: password=secret")) == "unknown", "Reject ambiguous category prefixes");
		TestInitializationAndIsolation();
		TestErrorHandling();
		TestSynchronousOnly();
		TestWebAssemblyErrors();
		TestTermination();
		TestLifetimeAndReentrancy();
		std::cout << "PASS: V8 " << v8::V8::GetVersion()
				  << " runtime, isolation, failclosed errors, synchronous hooks, wasm validation/traps, "
					 "compilation/JS/wasm deadlines, reload lifetime\n";
		return EXIT_SUCCESS;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return EXIT_FAILURE;
	}
}
