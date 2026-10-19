/* SPDX-License-Identifier: MIT */

#include "../modules/js_runtime.h"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
void Check(bool condition, const std::string& message)
{
	if (!condition)
		throw std::runtime_error(message);
}

void Progress(unsigned int generation, const char* phase)
{
	using Clock = std::chrono::steady_clock;
	static const auto started = Clock::now();
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
	std::fprintf(stderr, "LIFECYCLE +%lldms generation=%u %s\n", static_cast<long long>(elapsed), generation, phase);
	std::fflush(stderr);
}

void RunGeneration(unsigned int generation)
{
	std::string error;
	{
		Progress(generation, "success: constructing runtime");
		js_runtime::Runtime runtime(10000);
		Progress(generation, "success: initializing script");
		std::string source = "globalThis.answer = 42; globalThis.bytes = new ArrayBuffer(4096);";
		Check(runtime.Initialize(source, "lifecycle.js", {}, error), error);
		Progress(generation, "success: checking persistent handle");
		source.assign("overwritten host source");
		v8::Global<v8::Value> retained;
		Check(runtime.WithContext(
				  [&](v8::Isolate* isolate, v8::Local<v8::Context> context)
				  {
					  v8::Local<v8::Value> value;
					  if (!context->Global()
							  ->Get(context, v8::String::NewFromUtf8Literal(isolate, "answer"))
							  .ToLocal(&value))
						  return false;
					  retained.Reset(isolate, value);
					  return value->IsInt32() && value.As<v8::Int32>()->Value() == 42;
				  },
				  error),
			error);
		// Caller-owned persistent handles must be reset before Runtime destruction.
		retained.Reset();
		Progress(generation, "success: disposing runtime");
	}
	Progress(generation, "success: disposed");
	{
		Progress(generation, "syntax: constructing runtime");
		js_runtime::Runtime failed(10000);
		Progress(generation, "syntax: initializing script");
		Check(!failed.Initialize("function (", "syntax.js", {}, error), "invalid syntax was accepted");
		Check(!failed.IsHealthy(), "failed runtime remained healthy");
		Progress(generation, "syntax: disposing runtime");
	}
	Progress(generation, "syntax: disposed");
	{
		Progress(generation, "timeout: constructing runtime");
		js_runtime::Runtime timed_out(25);
		Progress(generation, "timeout: executing infinite script");
		Check(!timed_out.Initialize("for (;;) {}", "timeout.js", {}, error), "timeout was accepted");
		Check(error.find("timed out") != std::string::npos, "operation did not time out: " + error);
		Progress(generation, "timeout: disposing runtime");
	}
	Progress(generation, "timeout: disposed");
	{
		Progress(generation, "fresh: constructing runtime");
		js_runtime::Runtime fresh(10000);
		Progress(generation, "fresh: initializing script");
		Check(fresh.Initialize("void 0;", "fresh.js", {}, error), error);
		Progress(generation, "fresh: disposing runtime");
	}
	Progress(generation, "fresh: disposed");
}
} // namespace

// Hidden internal symbols and -fno-gnu-unique ensure NODELETE, not a GNU-unique
// symbol's implicit retention, is the reason this state survives dlclose.
extern "C" __attribute__((visibility("default"))) unsigned int lifecycle_generation()
{
	static unsigned int completed = 0;
	try
	{
		if (completed == 0)
			v8::V8::SetFlagsFromString("--jitless --no-expose-wasm");
		Progress(completed + 1, "V8 flags set; starting generation");
		RunGeneration(completed + 1);
		return ++completed;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL: lifecycle generation: " << error.what() << '\n';
		return 0;
	}
}
