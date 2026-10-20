/* SPDX-License-Identifier: MIT */
#include "inspircd.h"
#include "timeutils.h"
#include "bounded_log.h"

// The persistent layout is an image contract, not an arbitrary writable path
// supplied by IRC configuration. In particular, do not follow /inspircd/logs,
// the compatibility symlink, while establishing this security boundary.
namespace
{
constexpr uid_t DaemonUID = 10000;
constexpr gid_t DaemonGID = 10000;

int OpenLogDirectory()
{
	constexpr int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
	if (::geteuid() != DaemonUID || ::getegid() != DaemonGID)
		throw bounded_log::Error("bounded logging requires the daemon identity");
	bounded_log::Descriptor directory(::open("/", flags));
	if (directory.Get() < 0)
		throw bounded_log::Error("cannot open bounded log directory");
	for (const char* component : {"inspircd", "persistent", "logs"})
	{
		struct stat parent{};
		if (::fstat(directory.Get(), &parent) || !S_ISDIR(parent.st_mode) ||
			(parent.st_uid != 0 && parent.st_uid != DaemonUID) || (parent.st_mode & 0022))
			throw bounded_log::Error("unsafe bounded log parent directory");
		const int child = ::openat(directory.Get(), component, flags);
		if (child < 0)
			throw bounded_log::Error("cannot open bounded log directory");
		directory.Reset(child);
	}
	const int result = ::fcntl(directory.Get(), F_DUPFD_CLOEXEC, 0);
	if (result < 0)
		throw bounded_log::Error("cannot retain bounded log directory");
	return result;
}

size_t ReadLimit(const std::shared_ptr<ConfigTag>& tag, const char* name, size_t fallback, size_t low, size_t high)
{
	try
	{
		return bounded_log::ParseLimit(tag->getString(name, std::to_string(fallback)), low, high);
	}
	catch (const bounded_log::Error& error)
	{
		throw CoreException(std::string("Invalid bounded logger configuration: ") + error.what());
	}
}
} // namespace

class BoundedMethod final : public Log::Method
{
	bounded_log::Writer writer;
	bool reportedtruncation = false;

public:
	BoundedMethod(int directory, size_t maximum, unsigned files)
		: writer(directory, DaemonUID, DaemonGID, maximum, files)
	{
	}

	~BoundedMethod() override
	{
		if (writer.TruncatedRecords() > 1)
			fprintf(stderr, "Bounded logging: %llu records truncated during this logger lifetime.\n",
				static_cast<unsigned long long>(writer.TruncatedRecords()));
	}

	void OnLog(time_t time, Log::Level level, const std::string& type, const std::string& message) override
	{
		// Defense in depth: RAWIO includes passwords and messages; some upstream
		// modules also write plaintext authentication details at DEBUG level.
		if (level > Log::Level::NORMAL || type == "USERINPUT" || type == "USEROUTPUT")
			return;
		try
		{
			const auto prefix = Time::ToString(time, Time::ISO_8601) + " " +
				(type.size() <= 128 ? type : std::string("OVERSIZED-TYPE")) + ": ";
			if (writer.Write(prefix, message) && !reportedtruncation)
			{
				reportedtruncation = true;
				fputs("Bounded logging: 1 record truncated; further notices suppressed until this logger closes.\n",
					stderr);
			}
		}
		catch (const bounded_log::Error& error)
		{
			// Never call Logs here: this runs inside the log manager's recursion
			// guard. It catches CoreException, disables this sink and warns opers.
			fprintf(stderr, "Bounded logging disabled: %s. Rehash after correcting the problem.\n", error.what());
			throw CoreException(std::string("Bounded logging disabled: ") + error.what());
		}
	}
};

class BoundedEngine final : public Log::Engine
{
public:
	explicit BoundedEngine(Module* Creator)
		: Log::Engine(Creator, "bounded")
	{
	}

	Log::MethodPtr Create(const std::shared_ptr<ConfigTag>& tag) override
	{
		try
		{
			const auto level = tag->getString("level", "normal");
			if (level != "critical" && level != "warning" && level != "normal")
				throw CoreException("The bounded logger permits only critical, warning, or normal levels");
			if (!tag->getString("target").empty())
				throw CoreException("The bounded logger uses a fixed target; omit log:target");
			const size_t maximum =
				ReadLimit(tag, "maxbytes", bounded_log::MaximumBytes, 1024, bounded_log::MaximumBytes);
			const unsigned files =
				static_cast<unsigned>(ReadLimit(tag, "files", bounded_log::MaximumFiles, 1, bounded_log::MaximumFiles));
			try
			{
				bounded_log::Descriptor directory(OpenLogDirectory());
				return std::make_shared<BoundedMethod>(directory.Get(), maximum, files);
			}
			catch (const bounded_log::Error& error)
			{
				throw CoreException(std::string("Cannot initialize bounded logging: ") + error.what());
			}
		}
		catch (const CoreException&)
		{
			// Rehash closes previous sinks before Create; Logs.Critical alone
			// can therefore disappear when this replacement cannot initialize.
			fputs("Bounded logging initialization failed; file logging is unavailable. Correct configuration and "
				  "rehash.\n",
				stderr);
			throw;
		}
	}
};

class ModuleLogBounded final : public Module
{
	BoundedEngine engine;

public:
	ModuleLogBounded()
		: Module(VF_NONE, "Provides private, strictly size-bounded persistent file logging.")
		, engine(this)
	{
	}
};

MODULE_INIT(ModuleLogBounded)
