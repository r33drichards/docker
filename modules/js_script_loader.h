/* SPDX-License-Identifier: MIT */

#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace js_scripts
{
enum class Authority
{
	User,
	Admin
};

struct SourceSpec
{
	enum class Kind
	{
		File,
		Directory
	};
	Kind kind;
	std::filesystem::path path;
	Authority authority = Authority::User;
	std::set<std::string> databases = {};
};

struct Limits
{
	std::size_t max_file_bytes = 1024 * 1024;
	std::size_t max_total_bytes = 16 * 1024 * 1024;
	std::size_t max_sources = 128;
};

struct Source
{
	std::filesystem::path canonical_path;
	std::string content;
	Authority authority = Authority::User;
	std::set<std::string> databases = {};
};

struct WasmSpec
{
	std::string name;
	std::filesystem::path path;
};

struct WasmAsset
{
	std::string name;
	std::filesystem::path canonical_path;
	std::string content;
};

class LoadError : public std::runtime_error
{
public:
	explicit LoadError(const std::string& message)
		: std::runtime_error("Script asset loader: " + message)
	{
	}
};

namespace detail
{
namespace fs = std::filesystem;

inline std::string Label(const fs::path& path)
{
	return "'" + path.string() + "'";
}

inline void ValidatePath(const fs::path& path)
{
	if (path.empty() || path.native().find('\0') != std::string::npos)
		throw LoadError("paths must be nonempty and contain no NUL bytes");
}

inline fs::path Canonical(const fs::path& path)
{
	std::error_code error;
	auto result = fs::canonical(path, error);
	if (error)
		throw LoadError("cannot resolve " + Label(path) + ": " + error.message());
	return result;
}

inline fs::path ConfigRoot(const fs::path& config_root, const Limits& limits)
{
	if (!limits.max_file_bytes || !limits.max_total_bytes || !limits.max_sources)
		throw LoadError("all limits must be positive");
	ValidatePath(config_root);
	const auto root = Canonical(config_root);
	std::error_code error;
	if (!fs::is_directory(root, error) || error)
		throw LoadError("config root is not an accessible directory: " + Label(root));
	return root;
}

class FileDescriptor
{
public:
	explicit FileDescriptor(const fs::path& path)
	{
		do
		{
			// O_NONBLOCK prevents a raced-in FIFO from hanging the loader.
			// O_NOFOLLOW refuses a raced-in final-component symlink.
			value = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
		} while (value < 0 && errno == EINTR);
		if (value < 0)
		{
			const std::error_code error(errno, std::generic_category());
			throw LoadError("cannot open " + Label(path) + ": " + error.message());
		}
	}

	~FileDescriptor() { ::close(value); }
	FileDescriptor(const FileDescriptor&) = delete;
	FileDescriptor& operator=(const FileDescriptor&) = delete;
	int Get() const { return value; }

private:
	int value = -1;
};

inline struct stat Stat(int fd, const fs::path& path)
{
	struct stat result{};
	if (::fstat(fd, &result) != 0)
	{
		const std::error_code error(errno, std::generic_category());
		throw LoadError("cannot inspect " + Label(path) + ": " + error.message());
	}
	return result;
}

inline std::string Read(const fs::path& path, const Limits& limits, std::size_t total)
{
	FileDescriptor fd(path);
	const auto before = Stat(fd.Get(), path);
	if (!S_ISREG(before.st_mode))
		throw LoadError("not a regular file: " + Label(path));
	if (before.st_size < 0)
		throw LoadError("invalid file size: " + Label(path));
	const auto size = static_cast<std::uintmax_t>(before.st_size);
	if (size > limits.max_file_bytes)
		throw LoadError("per-file byte limit exceeded: " + Label(path));
	if (size > limits.max_total_bytes - total)
		throw LoadError("total byte limit exceeded: " + Label(path));

	std::string content;
	std::array<char, 8192> buffer{};
	for (;;)
	{
		const auto file_remaining = limits.max_file_bytes - content.size();
		const auto total_remaining = limits.max_total_bytes - total - content.size();
		const auto remaining = std::min(file_remaining, total_remaining);
		// Read at most one byte beyond the bound, including when exactly full.
		const auto request = remaining < buffer.size() ? remaining + 1 : buffer.size();
		const auto count = ::read(fd.Get(), buffer.data(), request);
		if (count < 0)
		{
			if (errno == EINTR)
				continue;
			const std::error_code error(errno, std::generic_category());
			throw LoadError("cannot read " + Label(path) + ": " + error.message());
		}
		if (count == 0)
			break;
		const auto bytes = static_cast<std::size_t>(count);
		if (bytes > file_remaining)
			throw LoadError("per-file byte limit exceeded: " + Label(path));
		if (bytes > total_remaining)
			throw LoadError("total byte limit exceeded: " + Label(path));
		content.append(buffer.data(), bytes);
	}

	const auto after = Stat(fd.Get(), path);
	if (before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
		before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
		before.st_ctim.tv_nsec != after.st_ctim.tv_nsec || static_cast<std::uintmax_t>(after.st_size) != content.size())
		throw LoadError("file changed while reading: " + Label(path));
	return content;
}
} // namespace detail

// Build an owned candidate only. The caller can compile every candidate script
// and publish/swap the full set after success; failures leave its active set
// untouched. No scripts execute here. An empty spec list gives an empty set.
//
// Relative paths use config_root, which must be an existing directory. Explicit
// files must have a case-sensitive .js extension. Explicit file/directory
// symlinks resolve canonically. Directory discovery is nonrecursive, ignores
// entry symlinks and nonregular/non-.js entries, and sorts names by unsigned byte.
// A configured directory with no matching files is an error. Canonical path
// duplicates are errors (distinct hard-link paths are not treated as duplicates).
// Each source inherits its spec's authority, including all files in a directory.
// Authority is configuration metadata, not a JavaScript-controlled capability.
//
// This is Linux C++17 and assumes trusted configuration and script directories.
// Bounds apply to stored source bytes, including files that grow; a read probes
// at most one extra byte to detect overflow. Metadata checks catch ordinary
// concurrent edits, but this is not a filesystem-wide
// atomic snapshot or protection against adversarial directory replacement.
inline std::vector<Source> LoadSources(
	const std::vector<SourceSpec>& specs, const std::filesystem::path& config_root, const Limits& limits = {})
{
	namespace fs = std::filesystem;
	const auto root = detail::ConfigRoot(config_root, limits);
	std::error_code error;

	std::vector<Source> candidate;
	std::set<fs::path> seen;
	std::size_t total = 0;
	const auto append = [&](const fs::path& canonical_path, Authority authority, const std::set<std::string>& databases)
	{
		if (!seen.insert(canonical_path).second)
			throw LoadError("duplicate canonical path: " + detail::Label(canonical_path));
		if (candidate.size() >= limits.max_sources)
			throw LoadError("source count limit exceeded: " + detail::Label(canonical_path));
		auto content = detail::Read(canonical_path, limits, total);
		total += content.size();
		candidate.push_back(Source{canonical_path, std::move(content), authority, databases});
	};

	for (const auto& spec : specs)
	{
		detail::ValidatePath(spec.path);
		if (spec.authority != Authority::User && spec.authority != Authority::Admin)
			throw LoadError("unknown script authority: " + detail::Label(spec.path));
		const auto path = spec.path.is_absolute() ? spec.path : root / spec.path;
		if (spec.kind == SourceSpec::Kind::File)
		{
			if (path.extension() != ".js")
				throw LoadError("explicit file must have a .js extension: " + detail::Label(path));
			append(detail::Canonical(path), spec.authority, spec.databases);
			continue;
		}
		if (spec.kind != SourceSpec::Kind::Directory)
			throw LoadError("unknown source kind: " + detail::Label(path));
		const auto directory = detail::Canonical(path);
		if (!fs::is_directory(directory, error) || error)
			throw LoadError("not an accessible directory: " + detail::Label(directory));
		std::vector<fs::path> entries;
		fs::directory_iterator iterator(directory, error);
		if (error)
			throw LoadError("cannot enumerate " + detail::Label(directory) + ": " + error.message());
		for (const fs::directory_iterator end; iterator != end;)
		{
			const auto entry = iterator->path();
			if (entry.extension() == ".js")
			{
				const auto status = iterator->symlink_status(error);
				if (error)
					throw LoadError("cannot inspect " + detail::Label(entry) + ": " + error.message());
				if (fs::is_regular_file(status))
				{
					if (entries.size() >= limits.max_sources - candidate.size())
						throw LoadError("source count limit exceeded: " + detail::Label(directory));
					entries.push_back(entry);
				}
			}
			iterator.increment(error);
			if (error)
				throw LoadError("cannot enumerate " + detail::Label(directory) + ": " + error.message());
		}
		if (entries.empty())
			throw LoadError("directory contains no regular .js files: " + detail::Label(directory));
		std::sort(entries.begin(), entries.end(),
			[](const fs::path& left, const fs::path& right)
			{
				const auto a = left.filename().native();
				const auto b = right.filename().native();
				return std::lexicographical_compare(
					a.begin(), a.end(), b.begin(), b.end(), [](unsigned char x, unsigned char y) { return x < y; });
			});
		for (const auto& entry : entries)
			append(entry, spec.authority, spec.databases); // Canonical parent + filename; never resolve entry links.
	}
	return candidate;
}

// Load only explicitly configured local .wasm files, in configuration order.
// Logical names are case-sensitive ASCII [A-Za-z0-9_-]+, at most 64 bytes.
// Names and canonical paths must both be unique. Explicit symlinks may resolve;
// directories are rejected. Limits bound this asset set independently of JS.
//
// Assets have no authority of their own. The embedding layer may expose every
// asset as a Uint8Array; imports must retain the calling script's authority.
// This API only reads owned binary bytes. It validates neither Wasm magic nor
// module contents, executes nothing, and grants no runtime filesystem or WASI
// access. Engine compilation/validation and atomic publication belong to callers.
inline std::vector<WasmAsset> LoadWasmAssets(
	const std::vector<WasmSpec>& specs, const std::filesystem::path& config_root, const Limits& limits = {})
{
	const auto root = detail::ConfigRoot(config_root, limits);
	std::vector<WasmAsset> candidate;
	std::set<std::string> names;
	std::set<std::filesystem::path> paths;
	std::size_t total = 0;
	for (const auto& spec : specs)
	{
		const bool valid_name = !spec.name.empty() && spec.name.size() <= 64 &&
			std::all_of(spec.name.begin(), spec.name.end(),
				[](unsigned char byte)
				{
					return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
						(byte >= '0' && byte <= '9') || byte == '_' || byte == '-';
				});
		if (!valid_name)
			throw LoadError("Wasm name must be 1-64 ASCII letters, digits, underscores or hyphens");
		if (!names.insert(spec.name).second)
			throw LoadError("duplicate Wasm name: '" + spec.name + "'");
		detail::ValidatePath(spec.path);
		const auto path = spec.path.is_absolute() ? spec.path : root / spec.path;
		if (path.extension() != ".wasm")
			throw LoadError("explicit Wasm file must have a .wasm extension: " + detail::Label(path));
		const auto canonical_path = detail::Canonical(path);
		if (!paths.insert(canonical_path).second)
			throw LoadError("duplicate canonical path: " + detail::Label(canonical_path));
		if (candidate.size() >= limits.max_sources)
			throw LoadError("source count limit exceeded: " + detail::Label(canonical_path));
		auto content = detail::Read(canonical_path, limits, total);
		total += content.size();
		candidate.push_back(WasmAsset{spec.name, canonical_path, std::move(content)});
	}
	return candidate;
}
} // namespace js_scripts
