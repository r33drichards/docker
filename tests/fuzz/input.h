/* SPDX-License-Identifier: MIT */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace fuzz_test
{
inline void Require(bool condition, const char* message)
{
	if (!condition)
	{
		std::fprintf(stderr, "Fuzz invariant failed: %s\n", message);
		std::abort();
	}
}

class Input
{
	const std::uint8_t* data;
	std::size_t remaining;

public:
	Input(const std::uint8_t* bytes, std::size_t size)
		: data(bytes)
		, remaining(std::min<std::size_t>(size, 4096))
	{
	}
	unsigned Byte()
	{
		if (!remaining)
			return 0;
		--remaining;
		return *data++;
	}
	std::size_t Word()
	{
		const auto low = Byte();
		return low | (Byte() << 8);
	}
	std::string Take(std::size_t count)
	{
		count = std::min(count, remaining);
		if (!count)
			return {};
		std::string result(reinterpret_cast<const char*>(data), count);
		data += count;
		remaining -= count;
		return result;
	}
	std::string String() { return Take(Word()); }
};

struct Command
{
	std::string name;
	std::vector<std::string> parameters;
};

// u16 little-endian command length, command bytes, u8 parameter count modulo
// 18, then u16 little-endian length + bytes per parameter. Truncation is valid.
inline Command ReadCommand(const std::uint8_t* data, std::size_t size)
{
	Input input(data, size);
	Command command{input.String(), {}};
	const auto count = input.Byte() % 18;
	for (unsigned i = 0; i < count; ++i)
		command.parameters.push_back(input.String());
	return command;
}
} // namespace fuzz_test
