/* SPDX-License-Identifier: MIT */
#include "../modules/js_runtime.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace
{
std::string Read(const std::string& path)
{
	std::ifstream input(path);
	if (!input)
		throw std::runtime_error("cannot read " + path);
	return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool Evaluate(v8::Isolate* isolate, v8::Local<v8::Context> context, const std::string& source)
{
	v8::Local<v8::String> text;
	v8::Local<v8::Script> script;
	v8::Local<v8::Value> result;
	return v8::String::NewFromUtf8(isolate, source.data(), v8::NewStringType::kNormal, static_cast<int>(source.size()))
			   .ToLocal(&text) &&
		v8::Script::Compile(context, text).ToLocal(&script) && script->Run(context).ToLocal(&result) &&
		js_runtime::Runtime::RequireSynchronous(isolate, result);
}
} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 3)
			throw std::runtime_error("usage: js_replay host.js scenario.js [scenario.js ...]");
		std::string host = Read(argv[1]);
		int first = 2;
		std::string target;
		std::string preload;
		while (first < argc && std::string(argv[first]).rfind("--", 0) == 0)
		{
			const std::string option = argv[first++];
			if (first >= argc)
				throw std::runtime_error(option + " needs a path");
			if (option == "--script")
				target = Read(argv[first++]);
			else if (option == "--preload")
				preload += Read(argv[first++]) + "\n;\n";
			else
				throw std::runtime_error("unknown replay option: " + option);
		}
		if (first >= argc)
			throw std::runtime_error("at least one scenario required");
		host = preload + host;
		for (int index = first; index < argc; ++index)
		{
			js_runtime::Runtime runtime(1000);
			std::string error;
			if (!runtime.Initialize(host, argv[1], {}, error))
				throw std::runtime_error(error);
			if (!target.empty() &&
				!runtime.WithContext([&](v8::Isolate* isolate, v8::Local<v8::Context> context)
					{ return Evaluate(isolate, context, target); }, error))
				throw std::runtime_error(error);
			const auto scenario = Read(argv[index]);
			if (!runtime.WithContext([&](v8::Isolate* isolate, v8::Local<v8::Context> context)
					{ return Evaluate(isolate, context, scenario); }, error))
				throw std::runtime_error(std::string(argv[index]) + ": " + error);
			std::cout << "PASS embedded V8 replay " << argv[index] << '\n';
		}
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
	return 0;
}
