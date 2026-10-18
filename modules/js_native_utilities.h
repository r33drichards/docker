/*
 * Owned, primitive-only native callbacks for the audited JavaScript utilities.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "inspircd.h"
#include <v8.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

namespace js_native
{
namespace detail
{
	template <size_t N>
	inline void TypeError(v8::Isolate* isolate, const char (&message)[N])
	{
		isolate->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(isolate, message)));
	}

	template <size_t N>
	inline void RangeError(v8::Isolate* isolate, const char (&message)[N])
	{
		isolate->ThrowException(v8::Exception::RangeError(v8::String::NewFromUtf8Literal(isolate, message)));
	}

	template <size_t N>
	inline void Error(v8::Isolate* isolate, const char (&message)[N])
	{
		isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(isolate, message)));
	}

	// No C++ exception may unwind through a V8 callback. V8 termination and
	// pending JavaScript exceptions are left to the embedding hook's TryCatch.
	template <typename Callback>
	inline void Guard(v8::Isolate* isolate, Callback&& callback)
	{
		try
		{
			std::forward<Callback>(callback)();
		}
		catch (const std::bad_alloc&)
		{
			if (!isolate->IsExecutionTerminating())
				Error(isolate, "Native utility allocation failed");
		}
		catch (const std::exception&)
		{
			if (!isolate->IsExecutionTerminating())
				Error(isolate, "Native utility failed");
		}
		catch (...)
		{
			if (!isolate->IsExecutionTerminating())
				Error(isolate, "Native utility failed");
		}
	}

	inline bool ExactArity(const v8::FunctionCallbackInfo<v8::Value>& info, int expected)
	{
		if (info.Length() == expected)
			return true;
		TypeError(info.GetIsolate(), "Incorrect number of utility arguments");
		return false;
	}

	inline bool ServerAvailable(v8::Isolate* isolate)
	{
		if (ServerInstance)
			return true;
		Error(isolate, "IRC server is not available");
		return false;
	}

	// V8's default UTF-8 writer can encode lone surrogates. Reject malformed
	// UTF-16 first rather than silently replacing or leaking non-UTF-8 bytes.
	// A bounded stack buffer keeps this validation independent of input size.
	inline bool ReadString(v8::Isolate* isolate, v8::Local<v8::Value> value,
		std::string& result, bool rejectnul = false)
	{
		if (!value->IsString())
		{
			TypeError(isolate, "Utility arguments must be primitive strings");
			return false;
		}
		const auto input = value.As<v8::String>();
		const int length = input->Length();
		std::array<uint16_t, 256> buffer;
		bool highsurrogate = false;
		for (int offset = 0; offset < length;)
		{
			const int count = std::min(length - offset, static_cast<int>(buffer.size()));
			if (input->Write(isolate, buffer.data(), offset, count, v8::String::NO_NULL_TERMINATION) != count)
			{
				Error(isolate, "Could not read the utility string");
				return false;
			}
			for (int index = 0; index < count; ++index)
			{
				const uint16_t unit = buffer[index];
				if (highsurrogate)
				{
					if (unit < 0xdc00 || unit > 0xdfff)
					{
						TypeError(isolate, "Utility strings must contain well-formed UTF-16");
						return false;
					}
					highsurrogate = false;
				}
				else if (unit >= 0xd800 && unit <= 0xdbff)
					highsurrogate = true;
				else if (unit >= 0xdc00 && unit <= 0xdfff)
				{
					TypeError(isolate, "Utility strings must contain well-formed UTF-16");
					return false;
				}
				else if (rejectnul && unit == 0)
				{
					TypeError(isolate, "Matching arguments must not contain embedded NUL");
					return false;
				}
			}
			offset += count;
		}
		if (highsurrogate)
		{
			TypeError(isolate, "Utility strings must contain well-formed UTF-16");
			return false;
		}
		const int bytes = input->Utf8Length(isolate);
		if (bytes < 0)
		{
			RangeError(isolate, "Utility string exceeds native length limits");
			return false;
		}
		result.resize(static_cast<size_t>(bytes));
		if (!bytes)
			return true;
		int characters = 0;
		const int written = input->WriteUtf8(isolate, result.data(), bytes, &characters,
			v8::String::NO_NULL_TERMINATION);
		if (written != bytes || characters != length)
		{
			Error(isolate, "Could not convert the utility string to UTF-8");
			return false;
		}
		return true;
	}

	inline void ReturnString(const v8::FunctionCallbackInfo<v8::Value>& info, const std::string& value)
	{
		auto* isolate = info.GetIsolate();
		if (value.size() > static_cast<size_t>(v8::String::kMaxLength))
		{
			RangeError(isolate, "Utility result exceeds the JavaScript string limit");
			return;
		}
		v8::Local<v8::String> result;
		if (v8::String::NewFromUtf8(isolate, value.data(), v8::NewStringType::kNormal,
			static_cast<int>(value.size())).ToLocal(&result))
			info.GetReturnValue().Set(result);
		// A failed MaybeLocal propagates V8's exception/termination unchanged.
	}

	template <typename Integer>
	inline bool IsSafeInteger(Integer value)
	{
		static_assert(std::is_integral_v<Integer>, "Native time must have an integer representation");
		constexpr int64_t maxsafe = INT64_C(9007199254740991);
		if constexpr (std::numeric_limits<Integer>::digits < 53)
			return true;
		else if constexpr (std::is_signed_v<Integer>)
			return value >= static_cast<Integer>(-maxsafe) && value <= static_cast<Integer>(maxsafe);
		else
			return value <= static_cast<Integer>(maxsafe);
	}
} // namespace detail

inline void JsIrcdMatch(const v8::FunctionCallbackInfo<v8::Value>& info)
{
	detail::Guard(info.GetIsolate(), [&info]() {
		if (!detail::ExactArity(info, 2))
			return;
		std::string str, pattern;
		if (detail::ReadString(info.GetIsolate(), info[0], str, true)
			&& detail::ReadString(info.GetIsolate(), info[1], pattern, true))
			info.GetReturnValue().Set(InspIRCd::Match(str, pattern));
	});
}

inline void JsIrcdMatchCIDR(const v8::FunctionCallbackInfo<v8::Value>& info)
{
	detail::Guard(info.GetIsolate(), [&info]() {
		if (!detail::ExactArity(info, 2))
			return;
		std::string str, pattern;
		if (detail::ReadString(info.GetIsolate(), info[0], str, true)
			&& detail::ReadString(info.GetIsolate(), info[1], pattern, true))
			info.GetReturnValue().Set(InspIRCd::MatchCIDR(str, pattern));
	});
}

inline void JsIrcdIsNick(const v8::FunctionCallbackInfo<v8::Value>& info)
{
	detail::Guard(info.GetIsolate(), [&info]() {
		if (!detail::ExactArity(info, 1))
			return;
		std::string value;
		if (detail::ReadString(info.GetIsolate(), info[0], value) && detail::ServerAvailable(info.GetIsolate()))
			info.GetReturnValue().Set(ServerInstance->IsNick(value));
	});
}

inline void JsIrcdIsChannel(const v8::FunctionCallbackInfo<v8::Value>& info)
{
	detail::Guard(info.GetIsolate(), [&info]() {
		if (!detail::ExactArity(info, 1))
			return;
		std::string value;
		if (detail::ReadString(info.GetIsolate(), info[0], value) && detail::ServerAvailable(info.GetIsolate()))
			info.GetReturnValue().Set(ServerInstance->Channels.IsChannel(value));
	});
}

inline void JsIrcdStripColor(const v8::FunctionCallbackInfo<v8::Value>& info)
{
	detail::Guard(info.GetIsolate(), [&info]() {
		if (!detail::ExactArity(info, 1))
			return;
		std::string value_copy;
		if (!detail::ReadString(info.GetIsolate(), info[0], value_copy))
			return;
		InspIRCd::StripColor(value_copy);
		detail::ReturnString(info, value_copy);
	});
}

inline void JsIrcdTime(const v8::FunctionCallbackInfo<v8::Value>& info)
{
	detail::Guard(info.GetIsolate(), [&info]() {
		if (!detail::ExactArity(info, 0) || !detail::ServerAvailable(info.GetIsolate()))
			return;
		const auto seconds = ServerInstance->Time();
		if (!detail::IsSafeInteger(seconds))
		{
			detail::RangeError(info.GetIsolate(), "IRC server time exceeds the JavaScript safe integer range");
			return;
		}
		info.GetReturnValue().Set(static_cast<double>(seconds));
	});
}

inline void RegisterUtilities(v8::Isolate* isolate, v8::Local<v8::ObjectTemplate> target)
{
#define IRC_JS_BINDING(js_name, native_adapter, exact_arity) \
	target->Set(v8::String::NewFromUtf8Literal(isolate, js_name), \
		v8::FunctionTemplate::New(isolate, native_adapter, v8::Local<v8::Value>(), \
			v8::Local<v8::Signature>(), exact_arity, v8::ConstructorBehavior::kThrow), \
		static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete));
#include "js_generated/inspircd_bindings.inc"
#undef IRC_JS_BINDING
}
} // namespace js_native
