/* SPDX-License-Identifier: MIT */

#include "../modules/parsed_command.h"

#include <cassert>
#include <type_traits>

static_assert(std::is_same<decltype(ParsedCommand::command), std::string>::value,
	"The command must be owned, not a view or reference");
static_assert(std::is_same<decltype(ParsedCommand::parameters), std::vector<std::string>>::value,
	"The parameters must be owned strings, not views or references");

int main()
{
	const std::vector<std::string> expected{
		"target", "two  spaces :and a colon", "Unicode café ☕", "", "password-secret"};
	std::string command = "PRIVMSG";
	std::vector<std::string> parameters = expected;
	const ParsedCommand parsed{command, parameters};
	assert(parsed.command == "PRIVMSG");
	assert(parsed.parameters == expected);
	assert(parsed.Repr() ==
		R"(ParsedCommand{command="PRIVMSG", parameters=["target", "two  spaces :and a colon", "Unicode caf\xc3\xa9 \xe2\x98\x95", "", "password-secret"]})");

	// Mutating and freeing the original buffers must not alter the snapshot.
	command.assign("CHANGED");
	parameters[1].assign("changed");
	parameters.clear();
	parameters.shrink_to_fit();
	assert(parsed.command == "PRIVMSG");
	assert(parsed.parameters == expected);

	// The snapshot must also outlive inputs created in a nested scope.
	const auto retained = []
	{
		std::string local_command = "OPER";
		std::vector<std::string> local_parameters{"operator", "full-password-secret"};
		return ParsedCommand{local_command, local_parameters};
	}();
	assert(retained.command == "OPER");
	assert(retained.parameters == (std::vector<std::string>{"operator", "full-password-secret"}));

	const ParsedCommand empty{"VERSION", {}};
	assert(empty.parameters.empty());
	assert(empty.Repr() == R"(ParsedCommand{command="VERSION", parameters=[]})");

	const ParsedCommand controls{"X\"\r\n", {std::string("\\\t\0", 3), std::string(1, '\x7f')}};
	assert(controls.Repr() == R"(ParsedCommand{command="X\"\x0d\x0a", parameters=["\\\x09\x00", "\x7f"]})");

	// Every byte must survive an independent decoding of the quoted repr.
	std::string bytes;
	for (unsigned int byte = 0; byte < 256; ++byte)
		bytes += static_cast<char>(byte);
	const auto repr = ParsedCommand{"BYTES", {bytes}}.Repr();
	for (unsigned char byte : repr)
		assert(byte >= 0x20 && byte < 0x7f);
	std::string decoded;
	for (size_t index = repr.find('[') + 2; repr[index] != '"'; ++index)
	{
		char byte = repr[index];
		if (byte == '\\')
		{
			byte = repr[++index];
			if (byte == 'x')
			{
				byte = static_cast<char>(std::stoul(repr.substr(index + 1, 2), nullptr, 16));
				index += 2;
			}
		}
		decoded += byte;
	}
	assert(decoded == bytes);
}
