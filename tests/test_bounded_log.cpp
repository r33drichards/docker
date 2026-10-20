/* SPDX-License-Identifier: MIT */
#include <cerrno>
#include <unistd.h>
#include <cstdio>

static bool fail_write = false;
static bool partial_write = false;
static bool fail_rename = false;
static ssize_t TestWrite(int fd, const void* bytes, size_t length)
{
	if (fail_write)
	{
		errno = ENOSPC;
		return -1;
	}
	if (partial_write)
	{
		partial_write = false;
		fail_write = true;
		return ::write(fd, bytes, length > 3 ? 3 : length);
	}
	return ::write(fd, bytes, length);
}
static int TestRename(int olddir, const char* oldname, int newdir, const char* newname)
{
	if (fail_rename)
	{
		errno = EIO;
		return -1;
	}
	return ::renameat(olddir, oldname, newdir, newname);
}
#define write TestWrite
#define renameat TestRename
#include "../modules/bounded_log.h"
#undef write
#undef renameat

#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
static void Check(bool ok, const char* message)
{
	if (!ok)
		throw std::runtime_error(message);
}
template <typename Callable> static void Error(Callable action)
{
	try
	{
		action();
	}
	catch (const bounded_log::Error& error)
	{
		Check(std::string(error.what()).find("TOP-SECRET") == std::string::npos, "error disclosed content");
		return;
	}
	throw std::runtime_error("expected a bounded log error");
}
struct Directory
{
	fs::path path;
	int fd;
	Directory()
	{
		char pattern[] = "/tmp/bounded-log-tests-XXXXXX";
		const char* result = ::mkdtemp(pattern);
		Check(result != nullptr, "mkdtemp");
		path = result;
		fd = ::open(result, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		Check(fd >= 0, "open fixture directory");
	}
	~Directory()
	{
		::close(fd);
		std::error_code ignored;
		fs::remove_all(path, ignored);
	}
	void Seed(const std::string& name, const std::string& content)
	{
		std::ofstream(path / name, std::ios::binary) << content;
		Check(::chmod((path / name).c_str(), 0600) == 0, "private seed");
	}
	std::string Read(const std::string& name)
	{
		std::ifstream stream(path / name, std::ios::binary);
		return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
	}
	void Bounds(size_t maximum, size_t files)
	{
		size_t total = 0, count = 0;
		for (const auto& entry : fs::directory_iterator(path))
		{
			if (entry.path().filename() == "inspircd.log.lock")
				continue;
			Check(entry.is_regular_file(), "not a regular log");
			const auto size = entry.file_size();
			Check(size <= maximum, "per-file cap exceeded");
			total += size;
			++count;
		}
		Check(count <= files && total <= maximum * files, "aggregate cap exceeded");
	}
};

int main()
{
	try
	{
		Check(bounded_log::ParseLimit("10485760", 1024, bounded_log::MaximumBytes) == bounded_log::MaximumBytes,
			"limit parsing");
		for (const char* value : {"", "0", "6", "-1", "1x", " 1", "999999999999999999999999999999999999999"})
			Error([&] { bounded_log::ParseLimit(value, 1, 5); });
		{
			Directory dir;
			bounded_log::Writer writer(dir.fd, ::geteuid(), ::getegid(), 1024, 5);
			for (size_t i = 0; i < 500; ++i)
			{
				writer.Write("test: ", std::string(90, static_cast<char>('a' + i % 26)));
				dir.Bounds(1024, 5);
			}
			Check(fs::exists(dir.path / "inspircd.log.4"), "rotation did not retain four archives");
			Check(!fs::exists(dir.path / "inspircd.log.5"), "unexpected fifth archive");
			Error([&] { bounded_log::Writer duplicate(dir.fd, ::geteuid(), ::getegid(), 1024, 5); });
		}
		{
			Directory dir;
			bounded_log::Writer writer(dir.fd, ::geteuid(), ::getegid(), 1024, 1);
			Check(writer.Write("test: ", std::string(20000, 'x')), "long record not reported truncated");
			dir.Bounds(1024, 1);
			const auto data = dir.Read("inspircd.log");
			Check(data.size() == 1024 && data.find("[truncated]\n") != std::string::npos, "truncation marker/cap");
			writer.Write("test: ", "next\r\nline\tend");
			dir.Bounds(1024, 1);
			Check(dir.Read("inspircd.log") == "test: next  line end\n", "control character sanitizing");
			Check(writer.TruncatedRecords() == 1, "truncation count");
		}
		{
			Directory dir;
			dir.Seed("inspircd.log", std::string(1100, 'z'));
			Error([&] { bounded_log::Writer writer(dir.fd, ::geteuid(), ::getegid(), 1024, 2); });
			Check(dir.Read("inspircd.log") == std::string(1100, 'z'), "oversized history was modified");
		}
		for (const char* name : {"inspircd.log.2", "inspircd.log.5", "inspircd.log.999", "inspircd.log.unexpected"})
		{
			Directory dir;
			dir.Seed(name, "preserve history");
			Error([&] { bounded_log::Writer writer(dir.fd, ::geteuid(), ::getegid(), 1024, 2); });
			Check(dir.Read(name) == "preserve history", "excess history was modified");
		}
		{
			Directory dir;
			{
				bounded_log::Writer writer(dir.fd, ::geteuid(), ::getegid(), 1024, 2);
				writer.Write("test: ", "new");
			}
			{
				bounded_log::Writer reopened(dir.fd, ::geteuid(), ::getegid(), 1024, 2);
				reopened.Write("test: ", "rehash");
				Check(dir.Read("inspircd.log") == "test: new\ntest: rehash\n", "rehash did not append");
			}
		}
		{
			Directory dir;
			bounded_log::Writer writer(dir.fd, ::geteuid(), ::getegid(), bounded_log::MaximumBytes, 5);
			writer.Write(std::string(100000, 'p'), std::string(100000, 'm'));
			Check(dir.Read("inspircd.log").size() == bounded_log::MaximumRecord,
				"large configuration bypassed record cap");
		}
		{
			Directory dir;
			Error([&] { bounded_log::Writer writer(dir.fd, ::geteuid() + 1, ::getegid(), 1024, 5); });
			Error([&] { bounded_log::Writer writer(dir.fd, ::geteuid(), ::getegid() + 1, 1024, 5); });
			Error([&] { bounded_log::Writer writer(dir.fd, ::geteuid(), ::getegid(), 1023, 5); });
			Error([&] { bounded_log::Writer writer(dir.fd, ::geteuid(), ::getegid(), 1024, 0); });
			Error([&] { bounded_log::Writer writer(dir.fd, ::geteuid(), ::getegid(), 1024, 6); });
		}

		for (const char* name : {"inspircd.log", "inspircd.log.4", "inspircd.log.lock"})
		{
			Directory dir, outside;
			outside.Seed("target", "TOP-SECRET-UNCHANGED");
			Check(::symlink((outside.path / "target").c_str(), (dir.path / name).c_str()) == 0, "symlink fixture");
			Error([&] { bounded_log::Writer writer(dir.fd, ::geteuid(), ::getegid(), 1024, 2); });
			Check(outside.Read("target") == "TOP-SECRET-UNCHANGED", "symlink target changed");
		}
		{
			Directory dir, outside;
			outside.Seed("target", "TOP-SECRET-UNCHANGED");
			Check(::link((outside.path / "target").c_str(), (dir.path / "inspircd.log").c_str()) == 0,
				"hardlink fixture");
			Error([&] { bounded_log::Writer writer(dir.fd, ::geteuid(), ::getegid(), 1024, 5); });
			Check(outside.Read("target") == "TOP-SECRET-UNCHANGED", "hardlink target changed");
		}
		{
			Directory dir;
			Check(::mkfifo((dir.path / "inspircd.log").c_str(), 0600) == 0, "fifo fixture");
			Error([&] { bounded_log::Writer writer(dir.fd, ::geteuid(), ::getegid(), 1024, 5); });
		}
		{
			Directory dir;
			dir.Seed("inspircd.log", "TOP-SECRET");
			Check(::chmod((dir.path / "inspircd.log").c_str(), 0644) == 0, "chmod unsafe file");
			Error([&] { bounded_log::Writer writer(dir.fd, ::geteuid(), ::getegid(), 1024, 5); });
			Check(::chmod(dir.path.c_str(), 0755) == 0, "chmod unsafe directory");
			Error([&] { bounded_log::Writer writer(dir.fd, ::geteuid(), ::getegid(), 1024, 5); });
		}
		{
			Directory dir;
			bounded_log::Writer writer(dir.fd, ::geteuid(), ::getegid(), 1024, 5);
			partial_write = true;
			Error([&] { writer.Write("test: ", "TOP-SECRET"); });
			fail_write = false;
			dir.Bounds(1024, 5);
			Check(!writer.Enabled(), "write failure did not disable logger");
			Error([&] { writer.Write("test: ", "must not resume"); });
			Check(dir.Read("inspircd.log") == "tes", "failed logger wrote again");
		}
		{
			Directory dir;
			bounded_log::Writer writer(dir.fd, ::geteuid(), ::getegid(), 1024, 5);
			writer.Write("", std::string(1023, 'a'));
			fail_rename = true;
			Error([&] { writer.Write("", "TOP-SECRET"); });
			fail_rename = false;
			Check(!writer.Enabled(), "rotation failure did not disable logger");
			dir.Bounds(1024, 5);
		}
		{
			Directory dir, outside;
			bounded_log::Writer writer(dir.fd, ::geteuid(), ::getegid(), 1024, 5);
			writer.Write("", std::string(1023, 'a'));
			outside.Seed("target", "TOP-SECRET-UNCHANGED");
			Check(::symlink((outside.path / "target").c_str(), (dir.path / "inspircd.log.4").c_str()) == 0,
				"rotation symlink");
			Error([&] { writer.Write("", "rotate"); });
			Check(outside.Read("target") == "TOP-SECRET-UNCHANGED", "rotation followed symlink");
		}
		std::cout << "bounded logging tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
