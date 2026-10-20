/* SPDX-License-Identifier: MIT */
#pragma once

// Synchronous, bounded SQLite storage for an embedding. No SQLite pointers,
// borrowed column data, or filesystem paths cross the JavaScript boundary.
//
// The configured directory and its ancestors are trusted host configuration.
// They must not be changed by a hostile process running as the server's UID.
// This is not a filesystem sandbox against same-UID/root processes. Directory
// descriptors, ownership checks, and NOFOLLOW defend against other users and
// accidentally configured links. SQLite's own VFS needs a canonical pathname:
// SQLITE_OPEN_NOFOLLOW deliberately rejects /proc/self/fd directory symlinks.
//
// VM step/time budgets interrupt SQLite computation, not blocking filesystem
// syscalls. Keep this small local database on reliable local storage.

#include <sqlite3.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace js_sqlite
{
using Blob = std::vector<uint8_t>;
using Value = std::variant<std::nullptr_t, int64_t, double, std::string, Blob>;

struct Limits
{
	std::size_t max_sql_bytes = 64 * 1024;
	std::size_t max_value_bytes = 1024 * 1024;
	std::size_t max_result_rows = 1000;
	std::size_t max_result_bytes = 4 * 1024 * 1024;
	std::size_t max_database_bytes = 64 * 1024 * 1024;
	std::size_t max_databases = 32;
	std::size_t max_parameters = 128;
	std::size_t max_columns = 128;
	uint64_t max_vm_steps = 100000;
	unsigned max_execution_ms = 25;
};

class Error : public std::runtime_error
{
public:
	explicit Error(const std::string& message)
		: std::runtime_error("SQLite: " + message)
	{
	}
};

struct Result
{
	std::vector<std::string> columns;
	std::vector<std::vector<Value>> rows;
	int64_t changes = 0;
	int64_t last_insert_rowid = 0;
};

namespace detail
{
class Descriptor
{
	int fd = -1;

public:
	explicit Descriptor(int value)
		: fd(value)
	{
	}
	~Descriptor()
	{
		if (fd >= 0)
			::close(fd);
	}
	Descriptor(const Descriptor&) = delete;
	Descriptor& operator=(const Descriptor&) = delete;
	int Get() const { return fd; }
};

inline bool ValidName(const std::string& name)
{
	if (name.empty() || name.size() > 64)
		return false;
	for (unsigned char byte : name)
		if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
				byte == '_' || byte == '-'))
			return false;
	return true;
}

inline bool SameFile(const struct stat& left, const struct stat& right)
{
	return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

inline void CheckFile(const struct stat& status)
{
	if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || status.st_nlink != 1 || (status.st_mode & 0077))
		throw Error("database and sidecars must be owned, regular, singly linked files without group/other access");
}

// A non-writable directory whose parent is writable is not itself stable.
// Root-owned sticky directories (notably /tmp) protect our owned child entry.
inline void CheckAncestors(const std::filesystem::path& root)
{
	for (auto path = root.parent_path(); !path.empty(); path = path.parent_path())
	{
		struct stat status{};
		if (::lstat(path.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
			(status.st_uid != 0 && status.st_uid != ::geteuid()) ||
			((status.st_mode & 0022) && !(status.st_uid == 0 && (status.st_mode & S_ISVTX))))
			throw Error("database directory ancestors must be trusted and not writable by other users");
		if (path == path.root_path())
			break;
	}
}

inline bool AllowedFunction(const char* value)
{
	if (!value)
		return false;
	// An allowlist prevents application/extension functions with filesystem or
	// process side effects, including readfile/writefile/load_extension/eval.
	static const char* const allowed[] = {"abs", "avg", "changes", "char", "coalesce", "concat", "concat_ws", "count",
		"cume_dist", "date", "datetime", "dense_rank", "format", "first_value", "glob", "group_concat", "hex", "if",
		"ifnull", "iif", "instr", "json", "json_array", "json_array_length", "json_error_position", "json_extract",
		"json_group_array", "json_group_object", "json_insert", "json_object", "json_patch", "json_pretty",
		"json_quote", "json_remove", "json_replace", "json_set", "json_type", "json_valid", "jsonb", "jsonb_array",
		"jsonb_extract", "jsonb_group_array", "jsonb_group_object", "jsonb_insert", "jsonb_object", "jsonb_patch",
		"jsonb_remove", "jsonb_replace", "jsonb_set", "julianday", "lag", "last_insert_rowid", "last_value", "lead",
		"length", "like", "likelihood", "likely", "lower", "ltrim", "max", "min", "nth_value", "ntile", "nullif",
		"octet_length", "percent_rank", "printf", "quote", "random", "randomblob", "rank", "replace", "round",
		"row_number", "rtrim", "sign", "soundex", "sqlite_source_id", "sqlite_version", "strftime", "string_agg",
		"substr", "substring", "sum", "time", "timediff", "total", "total_changes", "trim", "typeof", "unicode",
		"unhex", "unixepoch", "unlikely", "upper", "zeroblob", "->", "->>",
		// SQLite uses these internally while compiling ordinary ALTER TABLE.
		"sqlite_rename_column", "sqlite_rename_table", "sqlite_rename_test", "sqlite_rename_quotefix",
		"sqlite_drop_column"};
	for (const char* name : allowed)
		if (sqlite3_stricmp(value, name) == 0)
			return true;
	return false;
}
} // namespace detail

class Root
{
	std::filesystem::path path;
	detail::Descriptor directory;
	struct stat identity{};
	Limits limits;

	static std::filesystem::path Canonical(const std::string& input)
	{
		if (input.empty() || input.find('\0') != std::string::npos)
			throw Error("database directory must be a nonempty path without NUL bytes");
		std::error_code error;
		auto result = std::filesystem::canonical(input, error);
		if (error)
			throw Error("database directory must already exist and be accessible");
		return result;
	}

	void ValidateLimits()
	{
		const auto intmax = static_cast<std::size_t>(std::numeric_limits<int>::max());
		if (!limits.max_sql_bytes || limits.max_sql_bytes > intmax - 1 || !limits.max_value_bytes ||
			limits.max_value_bytes > intmax || !limits.max_result_rows || !limits.max_result_bytes ||
			limits.max_database_bytes < 4096 || limits.max_database_bytes > intmax || !limits.max_databases ||
			!limits.max_parameters || limits.max_parameters > intmax || !limits.max_columns ||
			limits.max_columns > intmax || !limits.max_vm_steps || !limits.max_execution_ms)
			throw Error("invalid database limits");
	}

	std::size_t DatabaseCount() const
	{
		// Opening "." instead of dup() gives each scan its own directory offset.
		const int scanfd = ::openat(directory.Get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (scanfd < 0)
			throw Error("could not inspect database directory");
		DIR* raw = ::fdopendir(scanfd);
		if (!raw)
		{
			::close(scanfd);
			throw Error("could not inspect database directory");
		}
		std::unique_ptr<DIR, int (*)(DIR*)> scan(raw, ::closedir);
		std::size_t count = 0;
		errno = 0;
		while (auto* entry = ::readdir(scan.get()))
		{
			const std::string name(entry->d_name);
			if (name.size() > 7 && name.compare(name.size() - 7, 7, ".sqlite") == 0)
				++count;
			errno = 0;
		}
		if (errno)
			throw Error("could not read database directory");
		return count;
	}

public:
	explicit Root(const std::string& input, const Limits& options = {})
		: path(Canonical(input))
		, directory(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW))
		, limits(options)
	{
		ValidateLimits();
		if (directory.Get() < 0 || ::fstat(directory.Get(), &identity) != 0 || !S_ISDIR(identity.st_mode) ||
			identity.st_uid != ::geteuid() || (identity.st_mode & 0077))
			throw Error("database directory must be private, owned by the server and not writable or accessible by "
						"group/others");
		detail::CheckAncestors(path);
		if (DatabaseCount() > limits.max_databases)
			throw Error("database count limit exceeded");
	}

	Root(const Root&) = delete;
	Root& operator=(const Root&) = delete;
	const Limits& GetLimits() const { return limits; }
	const std::filesystem::path& Path() const { return path; }

	void Check() const
	{
		struct stat current{}, opened{};
		if (::lstat(path.c_str(), &current) != 0 || ::fstat(directory.Get(), &opened) != 0 ||
			!detail::SameFile(current, identity) || !detail::SameFile(opened, identity) || !S_ISDIR(current.st_mode) ||
			current.st_uid != ::geteuid() || (current.st_mode & 0077))
			throw Error("database directory changed or became unsafe");
		detail::CheckAncestors(path);
	}

	void CheckEntries(const std::string& name) const
	{
		Check();
		for (const char* suffix : {"", "-journal", "-wal", "-shm"})
		{
			const std::string file = name + ".sqlite" + suffix;
			struct stat status{};
			if (::fstatat(directory.Get(), file.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0)
			{
				if (errno == ENOENT && *suffix)
					continue;
				throw Error("could not inspect database file");
			}
			detail::CheckFile(status);
			if (!*suffix && status.st_size > static_cast<off_t>(limits.max_database_bytes))
				throw Error("database size limit exceeded");
		}
	}

	std::string Prepare(const std::string& name) const
	{
		if (!detail::ValidName(name))
			throw Error("database name must contain 1-64 ASCII letters, digits, underscores or hyphens");
		Check();
		const std::string file = name + ".sqlite";
		struct stat existing{};
		if (::fstatat(directory.Get(), file.c_str(), &existing, AT_SYMLINK_NOFOLLOW) != 0)
		{
			if (errno != ENOENT)
				throw Error("could not inspect database file");
			if (DatabaseCount() >= limits.max_databases)
				throw Error("database count limit exceeded");
			detail::Descriptor created(
				::openat(directory.Get(), file.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
			if (created.Get() < 0)
				throw Error("could not create database file");
			if (::fstat(created.Get(), &existing) != 0)
				throw Error("could not inspect new database file");
			detail::CheckFile(existing);
		}
		CheckEntries(name);
		// Open the existing file without following links, even before the SQLite
		// VFS gets its independent NOFOLLOW open.
		detail::Descriptor checked(::openat(directory.Get(), file.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW));
		struct stat status{};
		if (checked.Get() < 0 || ::fstat(checked.Get(), &status) != 0)
			throw Error("could not securely open database file");
		detail::CheckFile(status);
		if (!detail::SameFile(existing, status))
			throw Error("database file changed while opening");
		return (path / file).string();
	}
};

class Connection
{
	std::shared_ptr<Root> root;
	std::string name;
	sqlite3* database = nullptr;
	bool host_transaction = false;
	// A logical host transaction outlives SQLite's implicit rollback on FULL,
	// INTERRUPT or a RAISE/ON CONFLICT ROLLBACK. Do not let a caught error turn
	// later callback writes into independent autocommit statements.
	bool transaction_scope = false;
	bool budget_exceeded = false;
	uint64_t progress_steps = 0;
	std::chrono::steady_clock::time_point deadline;
	static constexpr int progress_interval = 100;

	class Statement
	{
	public:
		sqlite3_stmt* value = nullptr;
		~Statement()
		{
			if (value)
				sqlite3_finalize(value);
		}
		Statement() = default;
		Statement(const Statement&) = delete;
		Statement& operator=(const Statement&) = delete;
	};

	class Budget
	{
		Connection& connection;

	public:
		explicit Budget(Connection& input)
			: connection(input)
		{
			connection.budget_exceeded = false;
			connection.progress_steps = 0;
			connection.deadline = std::chrono::steady_clock::now() +
				std::chrono::milliseconds(connection.root->GetLimits().max_execution_ms);
			sqlite3_progress_handler(connection.database, progress_interval, &Progress, &connection);
		}
		~Budget()
		{
			if (connection.database)
				sqlite3_progress_handler(connection.database, 0, nullptr, nullptr);
		}
	};

	static int Progress(void* opaque) noexcept
	{
		auto& self = *static_cast<Connection*>(opaque);
		self.progress_steps += progress_interval;
		if (self.progress_steps >= self.root->GetLimits().max_vm_steps ||
			std::chrono::steady_clock::now() >= self.deadline)
		{
			self.budget_exceeded = true;
			return 1;
		}
		return 0;
	}

	static int Authorize(
		void* opaque, int action, const char* first, const char* second, const char* schema, const char*) noexcept
	{
		auto& self = *static_cast<Connection*>(opaque);
		if (schema && std::strcmp(schema, "main") != 0 && std::strcmp(schema, "temp") != 0)
			return SQLITE_DENY;
		switch (action)
		{
		case SQLITE_CREATE_INDEX:
		case SQLITE_CREATE_TABLE:
		case SQLITE_CREATE_TEMP_INDEX:
		case SQLITE_CREATE_TEMP_TABLE:
		case SQLITE_CREATE_TEMP_TRIGGER:
		case SQLITE_CREATE_TEMP_VIEW:
		case SQLITE_CREATE_TRIGGER:
		case SQLITE_CREATE_VIEW:
		case SQLITE_DELETE:
		case SQLITE_DROP_INDEX:
		case SQLITE_DROP_TABLE:
		case SQLITE_DROP_TEMP_INDEX:
		case SQLITE_DROP_TEMP_TABLE:
		case SQLITE_DROP_TEMP_TRIGGER:
		case SQLITE_DROP_TEMP_VIEW:
		case SQLITE_DROP_TRIGGER:
		case SQLITE_DROP_VIEW:
		case SQLITE_INSERT:
		case SQLITE_READ:
		case SQLITE_SELECT:
		case SQLITE_UPDATE:
		case SQLITE_REINDEX:
		case SQLITE_ANALYZE:
		case SQLITE_RECURSIVE:
			return SQLITE_OK;
		case SQLITE_ALTER_TABLE:
			return first && (!std::strcmp(first, "main") || !std::strcmp(first, "temp")) ? SQLITE_OK : SQLITE_DENY;
		case SQLITE_FUNCTION:
			return detail::AllowedFunction(second) ? SQLITE_OK : SQLITE_DENY;
		case SQLITE_TRANSACTION:
		case SQLITE_SAVEPOINT:
			return self.host_transaction ? SQLITE_OK : SQLITE_DENY;
		// Includes PRAGMA, ATTACH, DETACH, CREATE/DROP VIRTUAL TABLE.
		default:
			return SQLITE_DENY;
		}
	}

	[[noreturn]] void Fail(const char* context) const
	{
		if (budget_exceeded)
			throw Error("statement execution budget exceeded");
		throw Error(std::string(context) + ": " + sqlite3_errmsg(database));
	}

	void RequireOpen() const
	{
		if (!database)
			throw Error("database handle is closed");
	}

	bool PhysicalTransaction() const { return database && !sqlite3_get_autocommit(database); }

	void CloseAfterFailure() noexcept
	{
		if (database)
			sqlite3_close_v2(database);
		database = nullptr;
		transaction_scope = false;
	}

	void RequireUsableTransaction() const
	{
		if (transaction_scope && !PhysicalTransaction())
			throw Error("transaction was aborted; rollback is required before further operations");
	}

	std::string ConfigureText(const char* sql)
	{
		Statement statement;
		if (sqlite3_prepare_v2(database, sql, -1, &statement.value, nullptr) != SQLITE_OK ||
			sqlite3_step(statement.value) != SQLITE_ROW)
			Fail("could not inspect database configuration");
		const auto* text = sqlite3_column_text(statement.value, 0);
		if (!text)
			Fail("could not inspect database configuration");
		return reinterpret_cast<const char*>(text);
	}

	int64_t ConfigureValue(const std::string& sql)
	{
		Statement statement;
		if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement.value, nullptr) != SQLITE_OK)
			Fail("could not configure database");
		const int result = sqlite3_step(statement.value);
		if (result != SQLITE_ROW && result != SQLITE_DONE)
			Fail("could not configure database");
		return result == SQLITE_ROW ? sqlite3_column_int64(statement.value, 0) : 0;
	}

	// Fixed host strings only, never user text. Used for transaction boundaries
	// and atomic per-statement savepoints. Cleanup disables exhausted budgets.
	void Command(const char* sql, bool cleanup = false)
	{
		if (cleanup)
			sqlite3_progress_handler(database, 0, nullptr, nullptr);
		const bool previous = host_transaction;
		host_transaction = true;
		const int result = sqlite3_exec(database, sql, nullptr, nullptr, nullptr);
		host_transaction = previous;
		if (result != SQLITE_OK)
			Fail("transaction failed");
	}

	void Configure()
	{
		const auto& limits = root->GetLimits();
		sqlite3_extended_result_codes(database, 1);
		sqlite3_busy_timeout(database, 0);
		int enabled = 0;
		if (sqlite3_db_config(database, SQLITE_DBCONFIG_DEFENSIVE, 1, &enabled) != SQLITE_OK || !enabled)
			throw Error("SQLite defensive mode is unavailable");
		if (sqlite3_db_config(database, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, &enabled) != SQLITE_OK || enabled)
			throw Error("SQLite trusted-schema protection is unavailable");
		if (sqlite3_db_config(database, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, &enabled) != SQLITE_OK || enabled)
			throw Error("could not disable SQLite extension loading");
		sqlite3_limit(database, SQLITE_LIMIT_LENGTH, static_cast<int>(limits.max_value_bytes));
		sqlite3_limit(database, SQLITE_LIMIT_SQL_LENGTH, static_cast<int>(limits.max_sql_bytes));
		sqlite3_limit(database, SQLITE_LIMIT_COLUMN, static_cast<int>(limits.max_columns));
		sqlite3_limit(database, SQLITE_LIMIT_EXPR_DEPTH, 100);
		sqlite3_limit(database, SQLITE_LIMIT_COMPOUND_SELECT, 32);
		sqlite3_limit(database, SQLITE_LIMIT_VDBE_OP, 25000);
		sqlite3_limit(database, SQLITE_LIMIT_FUNCTION_ARG, 32);
		sqlite3_limit(database, SQLITE_LIMIT_ATTACHED, 0);
		sqlite3_limit(database, SQLITE_LIMIT_LIKE_PATTERN_LENGTH, 4096);
		sqlite3_limit(database, SQLITE_LIMIT_VARIABLE_NUMBER, static_cast<int>(limits.max_parameters));
		sqlite3_limit(database, SQLITE_LIMIT_TRIGGER_DEPTH, 16);
		sqlite3_limit(database, SQLITE_LIMIT_WORKER_THREADS, 0);
		ConfigureValue("PRAGMA temp_store=MEMORY");
		ConfigureValue("PRAGMA mmap_size=0");
		ConfigureValue("PRAGMA cache_size=-2048");
		ConfigureValue("PRAGMA foreign_keys=ON");
		// Do not convert/checkpoint an existing WAL database while merely
		// opening a handle (which is also allowed during script validation).
		if (ConfigureText("PRAGMA journal_mode") != "delete")
			throw Error("database must already use DELETE journal mode");
		const auto page_size = ConfigureValue("PRAGMA page_size");
		if (page_size < 512 || page_size > 65536)
			throw Error("invalid SQLite page size");
		const auto max_pages = limits.max_database_bytes / static_cast<std::size_t>(page_size);
		if (!max_pages ||
			ConfigureValue("PRAGMA max_page_count=" + std::to_string(max_pages)) > static_cast<int64_t>(max_pages))
			throw Error("database size limit exceeded");
		const auto temp_page_size = ConfigureValue("PRAGMA temp.page_size");
		if (temp_page_size < 512 || temp_page_size > 65536)
			throw Error("invalid SQLite temporary page size");
		const auto max_temp_pages = limits.max_database_bytes / static_cast<std::size_t>(temp_page_size);
		if (!max_temp_pages ||
			ConfigureValue("PRAGMA temp.max_page_count=" + std::to_string(max_temp_pages)) >
				static_cast<int64_t>(max_temp_pages))
			throw Error("temporary database size limit exceeded");
		if (sqlite3_set_authorizer(database, &Authorize, this) != SQLITE_OK)
			throw Error("could not install SQLite authorization");
	}

	void Bind(sqlite3_stmt* statement, const std::vector<Value>& parameters)
	{
		const auto& limits = root->GetLimits();
		if (parameters.size() > limits.max_parameters ||
			sqlite3_bind_parameter_count(statement) != static_cast<int>(parameters.size()))
			throw Error("positional parameter count does not match SQL");
		std::size_t bytes = 0;
		for (std::size_t index = 0; index < parameters.size(); ++index)
		{
			const int position = static_cast<int>(index + 1);
			const char* parameter_name = sqlite3_bind_parameter_name(statement, position);
			if (parameter_name && parameter_name[0] != '?')
				throw Error("only positional SQL parameters are allowed");
			const Value& value = parameters[index];
			std::size_t length = sizeof(int64_t);
			if (const auto* text = std::get_if<std::string>(&value))
				length = text->size();
			else if (const auto* blob = std::get_if<Blob>(&value))
				length = blob->size();
			if (length > limits.max_value_bytes || bytes > limits.max_result_bytes ||
				length > limits.max_result_bytes - bytes)
				throw Error("bound value byte limit exceeded");
			bytes += length;
			int result = SQLITE_ERROR;
			if (std::holds_alternative<std::nullptr_t>(value))
				result = sqlite3_bind_null(statement, position);
			else if (const auto* integer = std::get_if<int64_t>(&value))
				result = sqlite3_bind_int64(statement, position, *integer);
			else if (const auto* number = std::get_if<double>(&value))
			{
				if (!std::isfinite(*number))
					throw Error("numeric parameters must be finite");
				result = sqlite3_bind_double(statement, position, *number);
			}
			else if (const auto* text = std::get_if<std::string>(&value))
				result = sqlite3_bind_text(
					statement, position, text->data(), static_cast<int>(text->size()), SQLITE_TRANSIENT);
			else if (const auto* blob = std::get_if<Blob>(&value))
				result =
					sqlite3_bind_blob(statement, position, blob->empty() ? "" : static_cast<const void*>(blob->data()),
						static_cast<int>(blob->size()), SQLITE_TRANSIENT);
			if (result != SQLITE_OK)
				Fail("could not bind parameter");
		}
	}

	void AddBytes(std::size_t& bytes, std::size_t extra) const
	{
		if (extra > root->GetLimits().max_result_bytes - bytes)
			throw Error("result byte limit exceeded");
		bytes += extra;
	}

	Value ReadValue(sqlite3_stmt* statement, int column, std::size_t& bytes)
	{
		switch (sqlite3_column_type(statement, column))
		{
		case SQLITE_NULL:
			AddBytes(bytes, 1);
			return nullptr;
		case SQLITE_INTEGER:
			AddBytes(bytes, sizeof(int64_t));
			return static_cast<int64_t>(sqlite3_column_int64(statement, column));
		case SQLITE_FLOAT:
			AddBytes(bytes, sizeof(double));
			return sqlite3_column_double(statement, column);
		case SQLITE_TEXT:
		case SQLITE_BLOB:
		{
			const bool text = sqlite3_column_type(statement, column) == SQLITE_TEXT;
			const void* data = text ? static_cast<const void*>(sqlite3_column_text(statement, column))
									: sqlite3_column_blob(statement, column);
			const auto size = static_cast<std::size_t>(sqlite3_column_bytes(statement, column));
			if (size > root->GetLimits().max_value_bytes)
				throw Error("result value byte limit exceeded");
			if (!data && (size || sqlite3_errcode(database) == SQLITE_NOMEM))
				Fail("could not copy result");
			AddBytes(bytes, size);
			if (text)
				return std::string(data ? static_cast<const char*>(data) : "", size);
			if (!size)
				return Blob{};
			const auto* first = static_cast<const uint8_t*>(data);
			return Blob(first, first + size);
		}
		default:
			throw Error("unsupported SQLite result type");
		}
	}

public:
	explicit Connection(std::shared_ptr<Root> input, const std::string& database_name)
		: root(std::move(input))
		, name(database_name)
	{
		if (!root)
			throw Error("database directory is not configured");
		// 3.41 also invokes the progress callback during complex preparation.
		if (sqlite3_libversion_number() < 3041000)
			throw Error("SQLite 3.41 or later is required");
		const std::string path = root->Prepare(name);
		const int result = sqlite3_open_v2(path.c_str(), &database,
			SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_PRIVATECACHE | SQLITE_OPEN_NOFOLLOW, nullptr);
		try
		{
			if (result != SQLITE_OK)
				Fail("could not open database");
			Budget budget(*this);
			Configure();
			root->CheckEntries(name);
		}
		catch (...)
		{
			if (database)
				sqlite3_close_v2(database);
			database = nullptr;
			throw;
		}
	}

	~Connection()
	{
		if (database)
			sqlite3_close_v2(database);
	}
	Connection(const Connection&) = delete;
	Connection& operator=(const Connection&) = delete;
	bool IsOpen() const { return database != nullptr; }
	bool InTransaction() const { return database && (transaction_scope || PhysicalTransaction()); }

	void Close()
	{
		if (!database)
			return;
		if (InTransaction())
			throw Error("cannot close a database during a transaction");
		if (sqlite3_close(database) != SQLITE_OK)
			Fail("could not close database");
		database = nullptr;
	}

	Result Execute(const std::string& sql, const std::vector<Value>& parameters = {}, bool collect_rows = true,
		bool read_only = false)
	{
		RequireOpen();
		RequireUsableTransaction();
		root->CheckEntries(name);
		const auto& limits = root->GetLimits();
		if (sql.empty() || sql.size() > limits.max_sql_bytes || sql.find('\0') != std::string::npos)
			throw Error("SQL must be nonempty, NUL-free, and within the SQL byte limit");
		Budget budget(*this);
		Statement statement;
		const char* tail = nullptr;
		if (sqlite3_prepare_v3(database, sql.c_str(), static_cast<int>(sql.size() + 1), SQLITE_PREPARE_NO_VTAB,
				&statement.value, &tail) != SQLITE_OK)
			Fail("could not prepare SQL");
		if (!statement.value)
			throw Error("SQL must contain one statement");
		// Preparation, not string splitting, handles quoted semicolons/comments.
		while (tail && tail < sql.c_str() + sql.size())
		{
			Statement extra;
			const char* next = nullptr;
			if (sqlite3_prepare_v3(database, tail, static_cast<int>(sql.c_str() + sql.size() - tail + 1),
					SQLITE_PREPARE_NO_VTAB, &extra.value, &next) != SQLITE_OK)
				Fail("could not prepare SQL tail");
			if (extra.value)
				throw Error("only one SQL statement is allowed");
			if (!next || next <= tail)
				break;
			tail = next;
		}
		const bool writes = sqlite3_stmt_readonly(statement.value) == 0;
		if (read_only && writes)
			throw Error("SQL writes are not allowed in this operation");
		Bind(statement.value, parameters);
		Result output;
		std::size_t result_bytes = 0;
		const int columns = sqlite3_column_count(statement.value);
		for (int column = 0; column < columns; ++column)
		{
			const char* value = sqlite3_column_name(statement.value, column);
			if (!value)
				Fail("could not read result column");
			const std::size_t length = std::strlen(value);
			AddBytes(result_bytes, length);
			output.columns.emplace_back(value, length);
		}
		bool savepoint = false;
		const auto total_before = sqlite3_total_changes64(database);
		try
		{
			if (writes)
			{
				Command("SAVEPOINT __js_sqlite_statement");
				savepoint = true;
			}
			std::size_t row_count = 0;
			for (;;)
			{
				const int result = sqlite3_step(statement.value);
				if (result == SQLITE_DONE)
					break;
				if (result != SQLITE_ROW)
					Fail("could not execute SQL");
				if (++row_count > limits.max_result_rows)
					throw Error("result row limit exceeded");
				std::vector<Value> row;
				if (collect_rows)
					row.reserve(static_cast<std::size_t>(columns));
				for (int column = 0; column < columns; ++column)
				{
					Value value = ReadValue(statement.value, column, result_bytes);
					if (collect_rows)
						row.emplace_back(std::move(value));
				}
				if (collect_rows)
					output.rows.emplace_back(std::move(row));
			}
			if (writes && sqlite3_total_changes64(database) != total_before)
				output.changes = sqlite3_changes64(database);
			output.last_insert_rowid = sqlite3_last_insert_rowid(database);
			// Finalize before releasing; RETURNING statements must have finished.
			const int finalized = sqlite3_finalize(statement.value);
			statement.value = nullptr;
			if (finalized != SQLITE_OK)
				Fail("could not finish SQL");
			if (savepoint)
				Command("RELEASE __js_sqlite_statement");
		}
		catch (...)
		{
			if (statement.value)
			{
				sqlite3_finalize(statement.value);
				statement.value = nullptr;
			}
			if (savepoint && PhysicalTransaction())
			{
				try
				{
					Command("ROLLBACK TO __js_sqlite_statement", true);
					Command("RELEASE __js_sqlite_statement", true);
				}
				catch (...)
				{
					// Never leave a failed statement's writes pending. If its
					// savepoint cannot be recovered, fail the entire transaction.
					try
					{
						Command("ROLLBACK", true);
					}
					catch (...)
					{
						CloseAfterFailure();
					}
				}
			}
			throw;
		}
		return output;
	}

	void Begin()
	{
		RequireOpen();
		root->CheckEntries(name);
		if (InTransaction())
			throw Error("nested transactions are not supported");
		Budget budget(*this);
		Command("BEGIN IMMEDIATE");
		transaction_scope = true;
	}

	void Commit()
	{
		RequireOpen();
		root->CheckEntries(name);
		if (!InTransaction())
			throw Error("no transaction is active");
		RequireUsableTransaction();
		Budget budget(*this);
		Command("COMMIT");
		transaction_scope = false;
	}

	void Rollback()
	{
		RequireOpen();
		try
		{
			if (PhysicalTransaction())
				Command("ROLLBACK", true);
		}
		catch (...)
		{
			// A handle whose rollback failed must not accept another callback.
			CloseAfterFailure();
			throw;
		}
		transaction_scope = false;
	}
};
} // namespace js_sqlite
