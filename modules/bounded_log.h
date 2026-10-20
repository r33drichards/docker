/* SPDX-License-Identifier: MIT */
#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

// Small, synchronous, POSIX file sink. Only the containing daemon writes this
// private directory. The native module supplies its fixed, trusted directory;
// tests supply an isolated descriptor. No paths or logged content enter errors.
namespace bounded_log
{
inline constexpr size_t MaximumBytes = 10 * 1024 * 1024;
inline constexpr unsigned MaximumFiles = 5;
inline constexpr size_t MaximumRecord = 16 * 1024;
inline constexpr const char* Basename = "inspircd.log";
inline constexpr const char* Lockname = "inspircd.log.lock";

class Error final : public std::runtime_error
{
public:
	explicit Error(const char* message)
		: std::runtime_error(message)
	{
	}
};

// Limits are deliberately strict decimal values, not getNum's silent fallback.
inline size_t ParseLimit(const std::string& input, size_t low, size_t high)
{
	if (input.empty())
		throw Error("bounded log limits must be decimal numbers");
	size_t result = 0;
	for (const unsigned char character : input)
	{
		if (character < '0' || character > '9')
			throw Error("bounded log limits must be decimal numbers");
		const size_t digit = character - '0';
		if (result > high / 10 || (result == high / 10 && digit > high % 10))
			throw Error("bounded log limit exceeds its hard maximum");
		result = result * 10 + digit;
	}
	if (result < low)
		throw Error("bounded log limit is below its supported minimum");
	return result;
}

class Descriptor final
{
	int value = -1;

public:
	explicit Descriptor(int fd = -1)
		: value(fd)
	{
	}
	~Descriptor() { Reset(); }
	Descriptor(const Descriptor&) = delete;
	Descriptor& operator=(const Descriptor&) = delete;
	int Get() const { return value; }
	void Reset(int fd = -1)
	{
		if (value >= 0)
			::close(value);
		value = fd;
	}
};

class Writer final
{
	Descriptor directory;
	Descriptor lease;
	Descriptor current;
	const uid_t owner;
	const gid_t group;
	const size_t maximum;
	const unsigned files;
	size_t used = 0;
	uint64_t truncated = 0;
	bool enabled = false;

	static std::string Filename(unsigned index)
	{
		return index ? std::string(Basename) + "." + std::to_string(index) : Basename;
	}

	void CheckDirectory() const
	{
		struct stat info{};
		if (::fstat(directory.Get(), &info) || !S_ISDIR(info.st_mode) || info.st_uid != owner || info.st_gid != group ||
			(info.st_mode & 07777) != 0700)
			throw Error("unsafe bounded log directory");
	}

	void CheckFile(const struct stat& info, bool lock = false) const
	{
		if (!S_ISREG(info.st_mode) || info.st_uid != owner || info.st_gid != group || info.st_nlink != 1 ||
			(info.st_mode & 07777) != 0600 || info.st_size < 0)
			throw Error("unsafe bounded log file");
		if (static_cast<uintmax_t>(info.st_size) > (lock ? 0 : maximum))
			throw Error("existing bounded log file exceeds its limit");
	}

	bool Inspect(const std::string& name, struct stat& info) const
	{
		if (::fstatat(directory.Get(), name.c_str(), &info, AT_SYMLINK_NOFOLLOW))
		{
			if (errno == ENOENT)
				return false;
			throw Error("cannot inspect bounded log file");
		}
		CheckFile(info, name == Lockname);
		return true;
	}

	// Reject, rather than delete or truncate, historical files outside the new
	// limits. Changing retention settings must never silently destroy history.
	void CheckNamespace() const
	{
		CheckDirectory();
		const int scanfd = ::openat(directory.Get(), ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (scanfd < 0)
			throw Error("cannot inspect bounded log directory");
		DIR* scan = ::fdopendir(scanfd);
		if (!scan)
		{
			::close(scanfd);
			throw Error("cannot inspect bounded log directory");
		}
		try
		{
			for (;;)
			{
				errno = 0;
				const auto* entry = ::readdir(scan);
				if (!entry)
				{
					if (errno)
						throw Error("cannot inspect bounded log directory");
					break;
				}
				const std::string name(entry->d_name);
				if (name != Basename && name.compare(0, sizeof("inspircd.log.") - 1, "inspircd.log.") != 0)
					continue;
				bool known = name == Lockname;
				for (unsigned index = 0; index < files; ++index)
					known |= name == Filename(index);
				if (!known)
					throw Error("existing bounded log archives exceed configured retention");
				struct stat info{};
				if (!Inspect(name, info))
					throw Error("bounded log directory changed during inspection");
			}
		}
		catch (...)
		{
			::closedir(scan);
			throw;
		}
		::closedir(scan);
	}

	void OpenCurrent()
	{
		struct stat before{};
		const bool existing = Inspect(Basename, before);
		const int fd = ::openat(directory.Get(), Basename,
			O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (existing ? 0 : O_CREAT | O_EXCL), 0600);
		if (fd < 0)
			throw Error("cannot open bounded log file");
		current.Reset(fd);
		struct stat info{};
		if (::fstat(fd, &info))
			throw Error("cannot inspect bounded log file");
		CheckFile(info);
		if (existing && (info.st_dev != before.st_dev || info.st_ino != before.st_ino))
			throw Error("bounded log file changed while opening");
		used = static_cast<size_t>(info.st_size);
	}

	void CheckCurrent()
	{
		struct stat descriptor{}, path{};
		if (::fstat(current.Get(), &descriptor))
			throw Error("cannot inspect active bounded log");
		CheckFile(descriptor);
		if (!Inspect(Basename, path) || descriptor.st_dev != path.st_dev || descriptor.st_ino != path.st_ino ||
			static_cast<size_t>(descriptor.st_size) != used)
			throw Error("active bounded log was modified externally");
	}

	void Remove(const std::string& name)
	{
		struct stat info{};
		if (Inspect(name, info) && ::unlinkat(directory.Get(), name.c_str(), 0))
			throw Error("cannot remove oldest bounded log");
	}

	void Rotate()
	{
		CheckNamespace();
		CheckCurrent();
		current.Reset();
		// Delete first: neither temporary files nor a transient sixth file can
		// exceed the total cap. A partial rotation remains within both caps.
		Remove(Filename(files - 1));
		for (unsigned index = files - 1; index > 0; --index)
		{
			const auto source = Filename(index - 1);
			struct stat info{};
			if (Inspect(source, info) &&
				::renameat(directory.Get(), source.c_str(), directory.Get(), Filename(index).c_str()))
				throw Error("cannot rotate bounded log");
		}
		OpenCurrent();
	}

	std::string Record(const std::string& prefix, const std::string& message, bool& shortened)
	{
		const size_t cap = std::min(maximum, MaximumRecord);
		// Avoid adding potentially unbounded input lengths or allocating an
		// unbounded intermediate string, even for hostile module log messages.
		shortened = prefix.size() > cap - 1 || message.size() > cap - 1 - std::min(prefix.size(), cap - 1);
		constexpr const char* marker = " [truncated]";
		const size_t budget = cap - 1 - (shortened ? sizeof(" [truncated]") - 1 : 0);
		std::string result;
		result.reserve(cap);
		for (const auto* part : {&prefix, &message})
		{
			for (const unsigned char c : *part)
			{
				if (result.size() == budget)
					break;
				result.push_back(c < 0x20 || c == 0x7f ? ' ' : static_cast<char>(c));
			}
		}
		if (shortened)
		{
			result += marker;
			if (truncated != UINT64_MAX)
				++truncated;
		}
		result += '\n';
		return result;
	}

public:
	Writer(int dir, uid_t uid, gid_t gid, size_t maxbytes, unsigned count)
		: directory(::fcntl(dir, F_DUPFD_CLOEXEC, 0))
		, owner(uid)
		, group(gid)
		, maximum(maxbytes)
		, files(count)
	{
		if (maxbytes < 1024 || maxbytes > MaximumBytes || count < 1 || count > MaximumFiles)
			throw Error("invalid bounded log limits");
		if (directory.Get() < 0)
			throw Error("cannot retain bounded log directory");
		CheckDirectory();
		struct stat before{};
		const bool existing = Inspect(Lockname, before);
		const int lockfd = ::openat(directory.Get(), Lockname,
			O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (existing ? 0 : O_CREAT | O_EXCL), 0600);
		if (lockfd < 0)
			throw Error("cannot open bounded log lock");
		lease.Reset(lockfd);
		struct stat info{};
		if (::fstat(lockfd, &info))
			throw Error("cannot inspect bounded log lock");
		CheckFile(info, true);
		if (existing && (info.st_dev != before.st_dev || info.st_ino != before.st_ino))
			throw Error("bounded log lock changed while opening");
		if (::flock(lockfd, LOCK_EX | LOCK_NB))
			throw Error("bounded log already has an active writer");
		CheckNamespace();
		OpenCurrent();
		enabled = true;
	}

	bool Enabled() const { return enabled; }
	uint64_t TruncatedRecords() const { return truncated; }

	bool Write(const std::string& prefix, const std::string& message)
	{
		if (!enabled)
			throw Error("bounded log sink is disabled");
		try
		{
			CheckDirectory();
			CheckCurrent();
			bool shortened = false;
			const auto record = Record(prefix, message, shortened);
			if (record.size() > maximum - used)
				Rotate();
			size_t done = 0;
			while (done < record.size())
			{
				const auto written = ::write(current.Get(), record.data() + done, record.size() - done);
				if (written < 0 && errno == EINTR)
					continue;
				if (written <= 0)
					throw Error("cannot write bounded log");
				done += static_cast<size_t>(written);
				used += static_cast<size_t>(written);
			}
			return shortened;
		}
		catch (...)
		{
			enabled = false;
			current.Reset();
			throw;
		}
	}
};
} // namespace bounded_log
