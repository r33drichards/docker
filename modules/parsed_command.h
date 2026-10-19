/* SPDX-License-Identifier: MIT */

#pragma once

#include <string>
#include <vector>

// An owned snapshot. Full parameter values can include credentials and private
// messages. Repr() escapes bytes for log safety; it does NOT redact them.
struct ParsedCommand
{
	std::string command;
	std::vector<std::string> parameters;

	std::string Repr() const
	{
		std::string result = "ParsedCommand{command=" + Quote(command) + ", parameters=[";
		for (size_t index = 0; index < parameters.size(); ++index)
		{
			if (index)
				result += ", ";
			result += Quote(parameters[index]);
		}
		return result + "]}";
	}

private:
	static std::string Quote(const std::string& value)
	{
		static const char hex[] = "0123456789abcdef";
		std::string result = "\"";
		for (unsigned char byte : value)
		{
			if (byte == '"' || byte == '\\')
			{
				result += '\\';
				result += static_cast<char>(byte);
			}
			else if (byte < 0x20 || byte >= 0x7f)
			{
				// Fixed-width byte escapes preserve arbitrary IRC bytes, including
				// UTF-8 and invalid UTF-8, without allowing embedded log lines.
				result += "\\x";
				result += hex[byte >> 4];
				result += hex[byte & 0x0f];
			}
			else
				result += static_cast<char>(byte);
		}
		return result + '"';
	}
};
