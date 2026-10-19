/* SPDX-License-Identifier: MIT */
#include "input.h"
#include "../../modules/parsed_command.h"

#include <charconv>
#include <string_view>
#include <system_error>

namespace
{
class Decoder
{
	std::string_view remaining;

public:
	explicit Decoder(const std::string& repr)
		: remaining(repr)
	{
	}
	void Literal(std::string_view literal)
	{
		fuzz_test::Require(remaining.substr(0, literal.size()) == literal, "repr grammar");
		remaining.remove_prefix(literal.size());
	}
	std::string Quoted()
	{
		Literal("\"");
		std::string result;
		while (!remaining.empty() && remaining.front() != '"')
		{
			char byte = remaining.front();
			remaining.remove_prefix(1);
			if (byte == '\\')
			{
				fuzz_test::Require(!remaining.empty(), "truncated escape");
				byte = remaining.front();
				remaining.remove_prefix(1);
				if (byte == 'x')
				{
					fuzz_test::Require(remaining.size() >= 2, "truncated hex escape");
					unsigned value = 0;
					const auto parsed = std::from_chars(remaining.data(), remaining.data() + 2, value, 16);
					fuzz_test::Require(
						parsed.ec == std::errc{} && parsed.ptr == remaining.data() + 2, "invalid hex escape");
					remaining.remove_prefix(2);
					byte = static_cast<char>(value);
				}
				else
					fuzz_test::Require(byte == '"' || byte == '\\', "unknown escape");
			}
			result.push_back(byte);
		}
		Literal("\"");
		return result;
	}
	bool StartsWith(char byte) const { return !remaining.empty() && remaining.front() == byte; }
	bool Empty() const { return remaining.empty(); }
};

fuzz_test::Command Decode(const std::string& repr)
{
	Decoder decoder(repr);
	decoder.Literal("ParsedCommand{command=");
	fuzz_test::Command result{decoder.Quoted(), {}};
	decoder.Literal(", parameters=[");
	while (decoder.StartsWith('"'))
	{
		result.parameters.push_back(decoder.Quoted());
		if (!decoder.StartsWith(','))
			break;
		decoder.Literal(", ");
	}
	decoder.Literal("]}");
	fuzz_test::Require(decoder.Empty(), "trailing repr bytes");
	return result;
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
	auto input = fuzz_test::ReadCommand(data, size);
	const auto expected = input;
	const ParsedCommand snapshot{input.name, input.parameters};
	input.name.assign("mutated");
	input.parameters.clear();
	const auto repr = snapshot.Repr();
	for (unsigned char byte : repr)
		fuzz_test::Require(byte >= 0x20 && byte <= 0x7e, "repr emitted an unsafe log byte");
	const auto decoded = Decode(repr);
	fuzz_test::Require(decoded.name == expected.name, "command round-trip lost bytes");
	fuzz_test::Require(decoded.parameters == expected.parameters, "parameter round-trip lost bytes");
	fuzz_test::Require(snapshot.Repr() == repr, "repr is nondeterministic");
	return 0;
}
