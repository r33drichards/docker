/* SPDX-License-Identifier: MIT */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace js_wire
{
// Validate converted/owned bytes, independently of V8. This intentionally keeps
// the rewrite boundary's conservative wire budget: one byte after the command
// and one byte for every parameter, including the optional trailing colon.
// UTF-8 validity belongs to the string conversion boundary; byte parameters
// are allowed to contain arbitrary non-NUL/non-CR/non-LF bytes.
inline bool Validate(const std::string& command, const std::vector<std::string>& parameters)
{
	if (command.empty() || command.size() > 32 || parameters.size() > 15 ||
		command.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos)
		return false;
	std::size_t remaining = 510 - command.size() - 1;
	for (std::size_t index = 0; index < parameters.size(); ++index)
	{
		const auto& text = parameters[index];
		if (text.find_first_of(std::string("\0\r\n", 3)) != std::string::npos ||
			(index + 1 < parameters.size() &&
				(text.empty() || text.front() == ':' || text.find(' ') != std::string::npos)))
			return false;
		// Compare before adding/subtracting, including for oversized byte arrays.
		if (text.size() >= remaining)
			return false;
		remaining -= text.size() + 1;
	}
	return true;
}
} // namespace js_wire
