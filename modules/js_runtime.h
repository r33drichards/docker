/* SPDX-License-Identifier: MIT */

#pragma once

#include <v8.h>
#include <libplatform/libplatform.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

namespace js_runtime
{
namespace detail
{
// Newer V8 releases dropped ScriptOrigin's isolate argument. Prefer the newer
// constructor when available without requiring a brittle version-number gate.
template <typename Origin = v8::ScriptOrigin>
Origin MakeScriptOrigin(v8::Isolate* isolate, v8::Local<v8::Value> name)
{
	if constexpr (std::is_constructible<Origin, v8::Local<v8::Value>>::value)
		return Origin(name);
	else
		return Origin(isolate, name);
}

// V8 has process lifetime, not IRC-module lifetime. Deliberately retain the
// platform and never call V8::Dispose/DisposePlatform. The containing shared
// object MUST be linked NODELETE so its code and this singleton survive unload.
// This embedder assumes it is the sole owner of V8 process initialization.
inline void EnsurePlatform()
{
	struct State
	{
		v8::Platform* platform = nullptr;
		bool ready = false;

		State()
		{
			if (!v8::V8::InitializeICUDefaultLocation(nullptr))
				return;
			platform = v8::platform::NewDefaultPlatform(1).release();
			v8::V8::InitializePlatform(platform);
			ready = v8::V8::Initialize();
		}
	};
	// A failed initialization is remembered, not retried against partially
	// initialized process-global V8 state. A successful state intentionally leaks.
	static const State* const state = new State;
	if (!state->ready)
		throw std::runtime_error("V8 process initialization failed");
}

// There is exactly one watchdog per active operation. Finish and termination
// synchronize on the same mutex, and the thread is joined before returning or
// touching isolate lifetime. No detached timer can terminate a later operation
// or race an isolate's disposal. The watchdog only calls TerminateExecution;
// it never enters the isolate or touches host/IRC data.
class Deadline
{
	using Clock = std::chrono::steady_clock;
	v8::Isolate* const isolate;
	const Clock::time_point deadline;
	std::mutex mutex;
	std::condition_variable wake;
	bool finished = false;
	bool expired = false;
	std::thread worker;

public:
	Deadline(v8::Isolate* target, std::chrono::milliseconds budget)
		: isolate(target)
		, deadline(Clock::now() + budget)
		, worker([this]
		{
			std::unique_lock<std::mutex> lock(mutex);
			if (!wake.wait_until(lock, deadline, [this] { return finished; }))
			{
				expired = true;
				isolate->TerminateExecution();
			}
		})
	{
	}

	~Deadline() { Finish(); }
	Deadline(const Deadline&) = delete;
	Deadline& operator=(const Deadline&) = delete;

	bool Finish()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (!finished)
			{
				// Scheduler delay must not turn an over-budget operation into
				// success merely because the worker has not run yet.
				expired = expired || Clock::now() >= deadline;
				finished = true;
			}
		}
		wake.notify_one();
		if (worker.joinable())
			worker.join();
		return expired;
	}
};
} // namespace detail

// One trusted administrator script per isolate/context. This is NOT a sandbox
// for hostile code: V8/native bugs and out-of-memory can terminate the daemon.
// heap_limit_mb sets V8's old-generation threshold, not a hard RSS/ArrayBuffer
// limit. Watchdogs interrupt JS/wasm, but cannot preempt blocking native calls.
//
// Create, use, and destroy on one owning thread. All JS execution, getters,
// conversions and host registration belong inside Initialize/WithContext.
// Persistent handles owned by callers must be reset before Runtime destruction.
// Native V8 callbacks must catch C++ exceptions themselves; C++ unwinding through
// JIT frames is not supported. Callers must discard queued host side effects
// whenever an operation fails. No microtask checkpoints/event loop are run.
class Runtime
{
public:
	using Action = std::function<bool(v8::Isolate*, v8::Local<v8::Context>)>;

private:
	const std::chrono::milliseconds timeout;
	const std::thread::id owner = std::this_thread::get_id();
	std::unique_ptr<v8::ArrayBuffer::Allocator> allocator;
	v8::Isolate* isolate = nullptr;
	v8::Global<v8::Context> context;
	std::string source_name;
	bool healthy = true;
	bool active = false;
	bool initialized = false;

	std::string Diagnostic(const char* message) const
	{
		// Never include a script-controlled thrown value or exception message:
		// those can contain passwords, IRC content, or unsafe toString methods.
		return source_name.empty() ? message : std::string(message) + " (" + source_name + ")";
	}

public:
	explicit Runtime(unsigned int timeout_ms, std::size_t heap_limit_mb = 32)
		: timeout(timeout_ms)
	{
		constexpr std::size_t mib = 1024 * 1024;
		if (timeout_ms == 0)
			throw std::invalid_argument("JavaScript timeout must be positive");
		if (heap_limit_mb == 0 || heap_limit_mb > std::numeric_limits<std::size_t>::max() / mib)
			throw std::invalid_argument("JavaScript heap threshold is invalid");
		detail::EnsurePlatform();
		allocator.reset(v8::ArrayBuffer::Allocator::NewDefaultAllocator());
		if (!allocator)
			throw std::runtime_error("V8 ArrayBuffer allocator initialization failed");
		v8::Isolate::CreateParams params;
		params.array_buffer_allocator = allocator.get();
		params.allow_atomics_wait = false;
		params.constraints.set_max_old_generation_size_in_bytes(heap_limit_mb * mib);
		isolate = v8::Isolate::New(params);
		if (!isolate)
			throw std::runtime_error("V8 isolate initialization failed");
		isolate->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
		isolate->SetAllowAtomicsWait(false);
		v8::Isolate::Scope isolate_scope(isolate);
		v8::HandleScope handles(isolate);
		context.Reset(isolate, v8::Context::New(isolate));
	}

	~Runtime()
	{
		// Destruction is only valid on the owning thread after the current
		// operation returned and after caller-owned persistent handles reset.
		context.Reset();
		isolate->Dispose();
	}

	Runtime(const Runtime&) = delete;
	Runtime& operator=(const Runtime&) = delete;
	Runtime(Runtime&&) = delete;
	Runtime& operator=(Runtime&&) = delete;

	v8::Isolate* GetIsolate() const { return isolate; }
	bool IsHealthy() const { return healthy; }

	// Returning a Promise is never a completed synchronous hook. Thenables are
	// plain return values: the host must not await or otherwise assimilate them.
	static bool RequireSynchronous(v8::Isolate* target, v8::Local<v8::Value> value)
	{
		if (value.IsEmpty())
			return false;
		if (!value->IsPromise())
			return true;
		target->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
			target, "Asynchronous JavaScript callbacks are not supported")));
		return false;
	}

	bool Initialize(const std::string& source, const std::string& filename,
		const Action& setup, std::string& error)
	{
		if (std::this_thread::get_id() != owner)
		{
			error = "JavaScript runtime used from a non-owning thread";
			return false;
		}
		if (initialized)
		{
			healthy = false;
			error = Diagnostic("JavaScript runtime already initialized");
			return false;
		}
		initialized = true;
		source_name = filename;
		return WithContext([&](v8::Isolate* target, v8::Local<v8::Context> local_context)
		{
			if (setup && !setup(target, local_context))
				return false;
			if (source.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
				|| filename.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
				return false;
			v8::Local<v8::String> code;
			v8::Local<v8::String> name;
			if (!v8::String::NewFromUtf8(target, source.data(), v8::NewStringType::kNormal,
				static_cast<int>(source.size())).ToLocal(&code)
				|| !v8::String::NewFromUtf8(target, filename.data(), v8::NewStringType::kNormal,
					static_cast<int>(filename.size())).ToLocal(&name))
				return false;
			auto origin = detail::MakeScriptOrigin(target, name);
			v8::Local<v8::Script> script;
			v8::Local<v8::Value> result;
			return v8::Script::Compile(local_context, code, &origin).ToLocal(&script)
				&& script->Run(local_context).ToLocal(&result)
				&& RequireSynchronous(target, result);
		}, error);
	}

	bool WithContext(const Action& action, std::string& error)
	{
		error.clear();
		if (std::this_thread::get_id() != owner)
		{
			error = "JavaScript runtime used from a non-owning thread";
			return false;
		}
		if (!healthy)
		{
			error = Diagnostic("JavaScript runtime is disabled until reload");
			return false;
		}
		if (active)
		{
			healthy = false;
			error = Diagnostic("Reentrant JavaScript runtime use is not supported");
			return false;
		}
		active = true;
		struct ResetActive
		{
			bool& value;
			~ResetActive() { value = false; }
		} reset_active{active};

		try
		{
			v8::Isolate::Scope isolate_scope(isolate);
			v8::HandleScope handles(isolate);
			auto local_context = context.Get(isolate);
			v8::Context::Scope context_scope(local_context);
			v8::TryCatch caught(isolate);
			detail::Deadline deadline(isolate, timeout);
			bool success = false;
			try
			{
				success = action && action(isolate, local_context);
			}
			catch (...)
			{
				error = Diagnostic("JavaScript host operation threw a C++ exception");
			}
			const bool expired = deadline.Finish();
			const bool terminated = caught.HasTerminated() || isolate->IsExecutionTerminating();
			if (expired || terminated)
				error = Diagnostic("JavaScript execution timed out");
			else if (caught.HasCaught())
				error = Diagnostic("JavaScript exception");
			else if (error.empty() && (!success || !healthy))
				error = Diagnostic("JavaScript host operation failed");
			if (!error.empty())
			{
				healthy = false;
				// All JS frames are gone and the watchdog is joined. Clear the
				// termination state only for orderly destruction, never reuse.
				if (expired || terminated)
					isolate->CancelTerminateExecution();
				return false;
			}
			return true;
		}
		catch (...)
		{
			// Includes failure to create the watchdog: never run unbounded.
			healthy = false;
			error = Diagnostic("JavaScript runtime operation failed");
			return false;
		}
	}
};
} // namespace js_runtime
