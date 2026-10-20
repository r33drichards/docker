/* SPDX-License-Identifier: MIT */
#include <sqlite3.h>
#include <cstring>

// Exercise failed rollback without exposing a production fault-injection API.
static bool reject_rollback = false;
static int rollback_error = SQLITE_IOERR;
static int FaultExec(
	sqlite3* db, const char* sql, int (*callback)(void*, int, char**, char**), void* argument, char** error)
{
	if (reject_rollback && std::strncmp(sql, "ROLLBACK", 8) == 0)
		return rollback_error;
	return sqlite3_exec(db, sql, callback, argument, error);
}
#define sqlite3_exec FaultExec
#include "../modules/js_sqlite.h"
#undef sqlite3_exec

#include <fstream>
#include <iostream>
#include <type_traits>

namespace fs = std::filesystem;
using js_sqlite::Blob;
using js_sqlite::Connection;
using js_sqlite::Error;
using js_sqlite::Limits;
using js_sqlite::Root;
using js_sqlite::Value;

static_assert(std::is_same<decltype(js_sqlite::Result::rows), std::vector<std::vector<Value>>>::value,
	"SQLite results must own their values");

static void Check(bool condition, const std::string& message)
{
	if (!condition)
		throw std::runtime_error(message);
}

template <typename Action> static void ExpectError(const std::string& fragment, Action action)
{
	try
	{
		action();
	}
	catch (const Error& error)
	{
		Check(std::string(error.what()).find(fragment) != std::string::npos,
			"wrong error: " + std::string(error.what()) + "; expected: " + fragment);
		return;
	}
	throw std::runtime_error("expected SQLite error: " + fragment);
}

struct TemporaryDirectory
{
	fs::path path;
	TemporaryDirectory()
	{
		char pattern[] = "/tmp/js-sqlite-tests-XXXXXX";
		const char* created = ::mkdtemp(pattern);
		if (!created)
			throw std::runtime_error("mkdtemp failed");
		path = created;
	}
	~TemporaryDirectory()
	{
		std::error_code error;
		fs::remove_all(path, error);
	}
	fs::path Directory(const std::string& name)
	{
		const auto output = path / name;
		fs::create_directory(output);
		Check(::chmod(output.c_str(), 0700) == 0, "chmod fixture failed");
		return output;
	}
};

static int64_t Integer(Connection& db, const std::string& sql)
{
	const auto result = db.Execute(sql);
	Check(result.rows.size() == 1 && result.rows[0].size() == 1, "expected one value");
	return std::get<int64_t>(result.rows[0][0]);
}

static void RawSql(const fs::path& file, const char* sql)
{
	sqlite3* db = nullptr;
	Check(sqlite3_open(file.c_str(), &db) == SQLITE_OK, "fixture SQLite open failed");
	const int result = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
	const std::string error(sqlite3_errmsg(db));
	sqlite3_close(db);
	Check(result == SQLITE_OK, "fixture SQL failed: " + error);
	Check(::chmod(file.c_str(), 0600) == 0, "private fixture permissions");
}

int main()
{
	try
	{
		TemporaryDirectory fixture;
		const auto directory = fixture.Directory("databases");
		auto root = std::make_shared<Root>(directory.string());
		Connection db(root, "main");
		Check(db.IsOpen() && !db.InTransaction(), "initial handle state");
		db.Execute("CREATE TABLE things(id INTEGER PRIMARY KEY, value, text, data)", {}, false);
		const std::string embedded("nul\0inside", 10);
		const Blob blob{0, 1, 127, 128, 255};
		const auto inserted = db.Execute(
			"INSERT INTO things VALUES(?, ?, ?, ?)", {INT64_C(9007199254740993), 1.25, embedded, blob}, false);
		Check(inserted.changes == 1 && inserted.last_insert_rowid == INT64_C(9007199254740993), "write metadata");
		const auto values = db.Execute("SELECT id, value, text, data FROM things");
		Check(values.columns == std::vector<std::string>({"id", "value", "text", "data"}), "column names");
		Check(std::get<int64_t>(values.rows[0][0]) == INT64_C(9007199254740993), "int64 precision");
		Check(std::get<double>(values.rows[0][1]) == 1.25, "double value");
		Check(std::get<std::string>(values.rows[0][2]) == embedded, "text embedded NUL");
		Check(std::get<Blob>(values.rows[0][3]) == blob, "blob value");
		const auto empty = db.Execute("SELECT ?, ?, ?", {nullptr, std::string{}, Blob{}});
		Check(std::holds_alternative<std::nullptr_t>(empty.rows[0][0]), "null binding");
		Check(std::get<std::string>(empty.rows[0][1]).empty(), "empty string binding");
		Check(std::get<Blob>(empty.rows[0][2]).empty(), "empty blob must not become SQL NULL");
		Check(db.Execute("SELECT 1 AS same, 2 AS same").columns == std::vector<std::string>({"same", "same"}),
			"duplicate column names preserved");
		Check(Integer(db, "SELECT count(*) FROM things") == 1, "count");
		Check(db.Execute("SELECT * FROM things").changes == 0, "read changes must not be stale");
		Check(db.Execute("CREATE TABLE other(value)").changes == 0, "DDL changes must not be stale");
		db.Execute("ALTER TABLE other ADD COLUMN added INTEGER");
		db.Execute("CREATE INDEX other_added ON other(added)");
		db.Execute("CREATE VIEW all_things AS SELECT * FROM things");
		Check(Integer(db, "SELECT count(*) FROM all_things") == 1, "ordinary schema operations");
		std::cout << "PASS: copied typed values, exact integers, metadata and schema operations\n";

		ExpectError("one SQL statement", [&] { db.Execute("INSERT INTO other(value) VALUES(1); SELECT 2"); });
		Check(Integer(db, "SELECT count(*) FROM other") == 0, "multiple statement rejection changed data");
		Check(Integer(db, "SELECT 7; -- trailing comment\n /* ; */") == 7, "trailing comments");
		Check(std::get<std::string>(db.Execute("SELECT 'a;b'").rows[0][0]) == "a;b", "quoted semicolon");
		ExpectError("one statement", [&] { db.Execute("-- comment only"); });
		ExpectError("NUL-free", [&] { db.Execute(std::string("SELECT 1\0; SELECT 2", 19)); });
		ExpectError("parameter count", [&] { db.Execute("SELECT ?", {}); });
		ExpectError("parameter count", [&] { db.Execute("SELECT 1", {INT64_C(2)}); });
		ExpectError("only positional", [&] { db.Execute("SELECT :name", {INT64_C(2)}); });
		Check(Integer(db, "SELECT 3") == 3, "parameter errors should not poison handle");
		ExpectError("finite", [&] { db.Execute("SELECT ?", {std::numeric_limits<double>::infinity()}); });
		ExpectError("finite", [&] { db.Execute("SELECT ?", {std::numeric_limits<double>::quiet_NaN()}); });
		ExpectError("writes are not allowed", [&] { db.Execute("DELETE FROM things", {}, false, true); });
		Check(Integer(db, "SELECT count(*) FROM things") == 1, "readonly mode mutated data");
		std::cout << "PASS: prepared single statements, binding validation and readonly mode\n";

		for (const char* sql : {"PRAGMA user_version", "PRAGMA writable_schema=ON", "PRAGMA temp_store=FILE",
				 "ATTACH DATABASE ':memory:' AS outside", "DETACH DATABASE main", "BEGIN", "COMMIT", "ROLLBACK",
				 "SAVEPOINT user_savepoint", "RELEASE user_savepoint",
				 "CREATE VIRTUAL TABLE forbidden USING fts5(text)", "SELECT load_extension('/tmp/anything')",
				 "SELECT readfile('/etc/passwd')", "SELECT writefile('/tmp/js-sqlite-must-not-exist', 'bad')",
				 "SELECT * FROM pragma_database_list", "VACUUM", "VACUUM INTO '/tmp/js-sqlite-must-not-exist'"})
			ExpectError("", [&] { db.Execute(sql); });
		Check(!fs::exists("/tmp/js-sqlite-must-not-exist"), "forbidden SQL wrote outside root");
		db.Execute("CREATE TEMP TABLE transient(value)");
		db.Execute("INSERT INTO transient VALUES(5)");
		Check(Integer(db, "SELECT value FROM transient") == 5, "memory temp table");
		std::cout << "PASS: authorization blocks filesystem, extension, virtual-table and transaction SQL\n";

		db.Begin();
		Check(db.InTransaction(), "begin transaction");
		ExpectError("nested", [&] { db.Begin(); });
		ExpectError("during a transaction", [&] { db.Close(); });
		db.Execute("INSERT INTO other(value) VALUES(10)");
		db.Rollback();
		db.Rollback();
		Check(!db.InTransaction() && Integer(db, "SELECT count(*) FROM other") == 0, "explicit rollback");
		db.Begin();
		db.Execute("INSERT INTO other(value) VALUES(11)");
		db.Commit();
		Check(!db.InTransaction() && Integer(db, "SELECT count(*) FROM other") == 1, "explicit commit");
		ExpectError("no transaction", [&] { db.Commit(); });
		db.Begin();
		db.Execute("INSERT INTO other(value) VALUES(12)");
		ExpectError("", [&] { db.Execute("INSERT INTO things(id) VALUES(9007199254740993)"); });
		Check(db.InTransaction(), "failed statement discarded enclosing transaction");
		db.Commit();
		Check(Integer(db, "SELECT count(*) FROM other") == 2, "successful sibling statement lost");
		{
			Connection pending(root, "abandoned");
			pending.Execute("CREATE TABLE t(value)");
			pending.Begin();
			pending.Execute("INSERT INTO t VALUES(1)");
		}
		Connection abandoned(root, "abandoned");
		Check(Integer(abandoned, "SELECT count(*) FROM t") == 0, "destructor failed to roll back transaction");
		{
			Connection rollback(root, "automatic-rollback");
			rollback.Execute("CREATE TABLE t(value)");
			rollback.Execute("CREATE TRIGGER stop BEFORE INSERT ON t BEGIN SELECT RAISE(ROLLBACK, 'stop'); END");
			rollback.Begin();
			ExpectError("stop", [&] { rollback.Execute("INSERT INTO t VALUES(1)"); });
			Check(rollback.InTransaction(), "automatic rollback lost logical host transaction");
			ExpectError("aborted", [&] { rollback.Execute("INSERT INTO t VALUES(2)"); });
			ExpectError("aborted", [&] { rollback.Commit(); });
			ExpectError("nested", [&] { rollback.Begin(); });
			rollback.Rollback();
			Check(!rollback.InTransaction(), "rollback failed to clear aborted scope");
		}
		std::cout << "PASS: commit, rollback, statement isolation, destructor and automatic rollback\n";

		{
			Connection locked(root, "main");
			db.Begin();
			const auto before = std::chrono::steady_clock::now();
			ExpectError("locked", [&] { locked.Execute("INSERT INTO other(value) VALUES(99)"); });
			Check(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(250), "busy handler waited");
			db.Rollback();
			locked.Execute("INSERT INTO other(value) VALUES(99)");
		}
		db.Close();
		Check(!db.IsOpen(), "close state");
		db.Close();
		ExpectError("closed", [&] { db.Execute("SELECT 1"); });
		Connection reopened(root, "main");
		Check(Integer(reopened, "SELECT count(*) FROM things") == 1, "persisted data disappeared");
		Check(std::get<std::string>(values.rows[0][2]) == embedded, "result data borrowed from closed handle");
		std::cout << "PASS: immediate busy failure, persistence and closed-handle behavior\n";

		Limits small;
		small.max_result_rows = 3;
		small.max_vm_steps = 5000;
		small.max_execution_ms = 1000;
		const auto small_directory = fixture.Directory("small");
		auto small_root = std::make_shared<Root>(small_directory.string(), small);
		Connection bounded(small_root, "bounded");
		bounded.Execute("CREATE TABLE t(value)");
		ExpectError("row limit",
			[&]
			{
				bounded.Execute(
					"WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<5) SELECT x FROM n");
			});
		ExpectError("row limit", [&] { bounded.Execute("INSERT INTO t VALUES(1),(2),(3),(4) RETURNING value"); });
		Check(Integer(bounded, "SELECT count(*) FROM t") == 0, "RETURNING row-limit failure committed writes");
		ExpectError(
			"row limit", [&] { bounded.Execute("INSERT INTO t VALUES(1),(2),(3),(4) RETURNING value", {}, false); });
		Check(
			Integer(bounded, "SELECT count(*) FROM t") == 0, "discarded RETURNING row-limit failure committed writes");
		bounded.Begin();
		bounded.Execute("INSERT INTO t VALUES(42)");
		ExpectError("row limit", [&] { bounded.Execute("INSERT INTO t VALUES(1),(2),(3),(4) RETURNING value"); });
		Check(bounded.InTransaction(), "row limit lost outer transaction");
		bounded.Commit();
		Check(Integer(bounded, "SELECT count(*) FROM t") == 1, "row-limit savepoint lost previous writes");
		ExpectError("budget exceeded",
			[&]
			{
				bounded.Execute("WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n) SELECT sum(x) FROM n");
			});
		Check(Integer(bounded, "SELECT 42") == 42, "budget failure poisoned subsequent queries");
		bounded.Begin();
		ExpectError("budget exceeded",
			[&]
			{
				bounded.Execute(
					"WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n) INSERT INTO t SELECT x FROM n");
			});
		Check(bounded.InTransaction(), "interrupted write lost logical transaction scope");
		ExpectError("aborted", [&] { bounded.Execute("INSERT INTO t VALUES(99)"); });
		ExpectError("aborted", [&] { bounded.Commit(); });
		bounded.Rollback();
		Check(Integer(bounded, "SELECT count(*) FROM t") == 1, "interrupted write persisted rows");
		std::cout << "PASS: result limits, RETURNING atomicity and interrupt budgets\n";
		for (int failure : {SQLITE_IOERR, SQLITE_NOMEM})
		{
			rollback_error = failure;
			auto fault_root =
				std::make_shared<Root>(fixture.Directory("rollback-faults-" + std::to_string(failure)).string(), small);
			Connection explicit_failure(fault_root, "explicit");
			explicit_failure.Execute("CREATE TABLE t(value)");
			explicit_failure.Begin();
			explicit_failure.Execute("INSERT INTO t VALUES(1)");
			reject_rollback = true;
			ExpectError("transaction failed", [&] { explicit_failure.Rollback(); });
			reject_rollback = false;
			Check(
				!explicit_failure.IsOpen() && !explicit_failure.InTransaction(), "failed rollback left handle usable");
			ExpectError("closed", [&] { explicit_failure.Execute("INSERT INTO t VALUES(2)"); });
			Connection explicit_check(fault_root, "explicit");
			Check(Integer(explicit_check, "SELECT count(*) FROM t") == 0, "failed rollback committed rows");

			Connection cleanup_failure(fault_root, "cleanup");
			cleanup_failure.Execute("CREATE TABLE t(value)");
			reject_rollback = true;
			ExpectError(
				"row limit", [&] { cleanup_failure.Execute("INSERT INTO t VALUES(1),(2),(3),(4) RETURNING value"); });
			reject_rollback = false;
			Check(!cleanup_failure.IsOpen(), "failed savepoint cleanup left handle usable");
			ExpectError("closed", [&] { cleanup_failure.Execute("INSERT INTO t VALUES(5)"); });
			Connection cleanup_check(fault_root, "cleanup");
			Check(Integer(cleanup_check, "SELECT count(*) FROM t") == 0, "failed savepoint cleanup committed rows");
		}
		std::cout << "PASS: rollback I/O/allocation failures close poisoned handles and prevent subsequent writes\n";

		{
			Limits limits;
			limits.max_sql_bytes = 1024;
			limits.max_value_bytes = 2048;
			limits.max_result_bytes = 4096;
			Connection limited(std::make_shared<Root>(fixture.Directory("bytes").string(), limits), "byte-limits");
			ExpectError("SQL byte limit", [&] { limited.Execute("SELECT 1 /*" + std::string(1024, 'a') + "*/"); });
			ExpectError("value byte limit", [&] { limited.Execute("SELECT ?", {std::string(2049, 'x')}); });
			ExpectError("value byte limit", [&] { limited.Execute("SELECT ?", {Blob(2049, 1)}); });
			ExpectError("", [&] { limited.Execute("SELECT zeroblob(2049)"); });
			ExpectError("result byte limit",
				[&]
				{
					limited.Execute(
						"SELECT zeroblob(2000) UNION ALL SELECT zeroblob(2000) UNION ALL SELECT zeroblob(2000)");
				});
		}
		{
			Limits limits;
			limits.max_database_bytes = 32768;
			Connection disk(std::make_shared<Root>(fixture.Directory("disk").string(), limits), "bounded");
			disk.Execute("CREATE TABLE t(value)");
			disk.Execute("INSERT INTO t VALUES(?)", {Blob(16000, 1)});
			ExpectError("full", [&] { disk.Execute("INSERT INTO t VALUES(?)", {Blob(16000, 1)}); });
			Check(Integer(disk, "SELECT count(*) FROM t") == 1, "disk-full write persisted or deleted rows");
		}
		{
			Limits limits;
			limits.max_databases = 2;
			const auto count_root = std::make_shared<Root>(fixture.Directory("count").string(), limits);
			Connection one(count_root, "one");
			Connection two(count_root, "two");
			Connection again(count_root, "one");
			ExpectError("count limit", [&] { Connection third(count_root, "three"); });
		}
		std::cout << "PASS: SQL/value/result bytes, database pages and database count limits\n";

		for (const std::string& name : std::vector<std::string>{"", "../escape", "/absolute", "file:escape",
				 "name.sqlite", std::string("a\0b", 3), std::string(65, 'x')})
			ExpectError("database name", [&] { Connection invalid(root, name); });
		const auto outside = fixture.path / "outside";
		{
			std::ofstream file(outside);
			file << "untouched";
		}
		fs::create_symlink(outside, directory / "symlink.sqlite");
		ExpectError("singly linked", [&] { Connection linked(root, "symlink"); });
		fs::create_hard_link(outside, directory / "hardlink.sqlite");
		ExpectError("singly linked", [&] { Connection linked(root, "hardlink"); });
		Check(::mkfifo((directory / "fifo.sqlite").c_str(), 0600) == 0, "FIFO setup");
		ExpectError("singly linked", [&] { Connection fifo(root, "fifo"); });
		fs::create_directory(directory / "directory.sqlite");
		ExpectError("singly linked", [&] { Connection invalid(root, "directory"); });
		for (const char* suffix : {"-journal", "-wal", "-shm"})
		{
			const std::string filename = "sidecar" + std::to_string(std::strlen(suffix));
			const auto link = directory / (filename + ".sqlite" + suffix);
			fs::create_symlink(outside, link);
			ExpectError("singly linked", [&] { Connection sidecar(root, filename); });
			fs::remove(link);
		}
		fs::create_symlink(outside, directory / "main.sqlite-journal");
		ExpectError("singly linked", [&] { reopened.Execute("SELECT 1"); });
		fs::remove(directory / "main.sqlite-journal");
		Check(Integer(reopened, "SELECT 1") == 1, "sidecar rejection poisoned handle");
		{
			std::ifstream file(outside);
			std::string contents;
			file >> contents;
			Check(contents == "untouched", "link target was touched");
		}
		const auto writable = fixture.Directory("writable");
		Check(::chmod(writable.c_str(), 0777) == 0, "chmod writable root");
		ExpectError("not writable", [&] { Root unsafe(writable.string()); });
		const auto nested = writable / "child";
		fs::create_directory(nested);
		Check(::chmod(nested.c_str(), 0700) == 0, "chmod child root");
		ExpectError("ancestors", [&] { Root unsafe(nested.string()); });
		const auto public_directory = fixture.Directory("public");
		Check(::chmod(public_directory.c_str(), 0755) == 0, "chmod public root");
		ExpectError("private", [&] { Root unsafe(public_directory.string()); });
		{
			const auto public_file = directory / "public.sqlite";
			std::ofstream file(public_file);
			file.close();
			Check(::chmod(public_file.c_str(), 0644) == 0, "chmod public database");
			ExpectError("group/other access", [&] { Connection public_db(root, "public"); });
		}
		const auto moved = fixture.Directory("moved");
		auto moved_root = std::make_shared<Root>(moved.string());
		Connection moving(moved_root, "data");
		fs::rename(moved, fixture.path / "moved-away");
		fs::create_directory(moved);
		ExpectError("directory changed", [&] { moving.Execute("SELECT 1"); });
		std::cout << "PASS: identifiers, symlinks, hardlinks, special files, sidecars and root identity\n";

		{
			const auto schema_root = fixture.Directory("untrusted-schema");
			RawSql(schema_root / "triggers.sqlite",
				"CREATE TABLE t(value);"
				"CREATE TRIGGER evil AFTER INSERT ON t BEGIN SELECT load_extension('/tmp/evil'); END;");
			Connection triggers(std::make_shared<Root>(schema_root.string()), "triggers");
			ExpectError("", [&] { triggers.Execute("INSERT INTO t VALUES(1)"); });
			Check(Integer(triggers, "SELECT count(*) FROM t") == 0, "unsafe stored trigger executed writes");
			RawSql(schema_root / "virtual.sqlite", "CREATE VIRTUAL TABLE t USING fts5(value);");
			Connection virtual_table(std::make_shared<Root>(schema_root.string()), "virtual");
			ExpectError("", [&] { virtual_table.Execute("SELECT * FROM t"); });
		}
		std::cout << "PASS: existing unsafe triggers and virtual tables are blocked\n";
		{
			const auto wal_root = fixture.Directory("wal");
			const auto file = wal_root / "existing.sqlite";
			RawSql(file, "PRAGMA journal_mode=WAL; CREATE TABLE t(value); INSERT INTO t VALUES(9);");
			ExpectError(
				"DELETE journal mode", [&] { Connection wal(std::make_shared<Root>(wal_root.string()), "existing"); });
			sqlite3* raw = nullptr;
			Check(sqlite3_open(file.c_str(), &raw) == SQLITE_OK, "open WAL verification");
			sqlite3_stmt* statement = nullptr;
			Check(sqlite3_prepare_v2(raw, "PRAGMA journal_mode", -1, &statement, nullptr) == SQLITE_OK,
				"prepare WAL verification");
			Check(sqlite3_step(statement) == SQLITE_ROW, "step WAL verification");
			Check(std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0))) == "wal",
				"open silently converted existing WAL database");
			sqlite3_finalize(statement);
			sqlite3_close(raw);
		}
		std::cout << "PASS: existing WAL database refused without journal-mode conversion\n";
		std::cout << "All SQLite backend tests passed (SQLite " << sqlite3_libversion() << ")\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
