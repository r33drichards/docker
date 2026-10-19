/* SPDX-License-Identifier: MIT */
#include "input.h"
#include "../../modules/js_wire_validation.h"

namespace
{
// Independent oracle: construct the conservative wire representation and
// inspect bytes/positions, rather than repeat the production budget arithmetic.
bool Reference(const fuzz_test::Command& input)
{
	if (input.name.size() < 1 || input.name.size() > 32 || input.parameters.size() > 15)
		return false;
	for (unsigned char byte : input.name)
		if (byte < 'A' || byte > 'Z')
			return false;
	std::string wire = input.name;
	if (input.parameters.empty())
		wire.push_back(' ');
	for (std::size_t i = 0; i < input.parameters.size(); ++i)
	{
		const auto& parameter = input.parameters[i];
		const bool trailing = i == input.parameters.size() - 1;
		if (!trailing && (parameter.empty() || parameter[0] == ':'))
			return false;
		wire.push_back(' ');
		if (trailing)
			wire.push_back(':');
		for (unsigned char byte : parameter)
		{
			if (byte == 0 || byte == 10 || byte == 13 || (!trailing && byte == 32))
				return false;
			wire.push_back(static_cast<char>(byte));
		}
	}
	return wire.size() <= 510;
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
	const auto input = fuzz_test::ReadCommand(data, size);
	fuzz_test::Require(js_wire::Validate(input.name, input.parameters) == Reference(input),
		"wire validation disagrees with independently serialized boundary");
	return 0;
}
