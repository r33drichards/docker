/* SPDX-License-Identifier: MIT */

#include "../modules/js_script_loader.h"

#include <fstream>
#include <iostream>
#include <type_traits>

namespace fs = std::filesystem;
using js_scripts::Authority;
using js_scripts::Limits;
using js_scripts::LoadError;
using js_scripts::LoadSources;
using js_scripts::LoadWasmAssets;
using js_scripts::Source;
using js_scripts::SourceSpec;
using Kind = SourceSpec::Kind;

static_assert(std::is_same<decltype(Source::content), std::string>::value, "Source bytes must be owned");
static_assert(std::is_same<decltype(Source::canonical_path), fs::path>::value, "Source paths must be owned");

static void Check(bool condition, const std::string& message)
{
	if (!condition)
		throw std::runtime_error(message);
}

static void Write(const fs::path& path, const std::string& bytes)
{
	std::ofstream file(path, std::ios::binary);
	file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	file.close();
	Check(!file.fail(), "could not write fixture " + path.string());
}

template <typename Action> static void ExpectError(const std::string& fragment, Action action)
{
	try
	{
		action();
	}
	catch (const LoadError& error)
	{
		Check(std::string(error.what()).find(fragment) != std::string::npos,
			"wrong error: " + std::string(error.what()) + "; expected " + fragment);
		return;
	}
	throw std::runtime_error("expected LoadError containing " + fragment);
}

int main(int argc, char** argv)
{
	try
	{
		Check(argc == 2, "usage: js_script_loader TEMP_DIRECTORY");
		const auto root = fs::absolute(fs::path(argv[1]) / "config root");
		const auto directory = root / "plugins";
		fs::create_directories(directory / "nested");
		fs::create_directories(directory / "not-a-file.js");
		Write(root / "before.js", "before");
		Write(root / "after.js", "after");
		Write(directory / "z.js", "z");
		Write(directory / "a.js", "a");
		Write(directory / "A.js", "A");
		Write(directory / (std::string(1, '\x80') + ".js"), "high byte");
		Write(directory / (std::string(1, '\x7f') + ".js"), "low byte");
		Write(directory / "ignored.txt", "ignored");
		Write(directory / "ignored.JS", "ignored");
		Write(directory / "nested" / "nested.js", "ignored");
		fs::create_symlink(root / "after.js", directory / "ignored-link.js");
		fs::create_symlink(root / "missing.js", directory / "broken-link.js");
		fs::create_directory_symlink(directory / "nested", directory / "linked-directory.js");
		Check(::mkfifo((directory / "ignored-fifo.js").c_str(), 0600) == 0, "could not create FIFO fixture");

		const auto ordered = LoadSources(
			{{Kind::File, "before.js"}, {Kind::Directory, "plugins"}, {Kind::File, root / "after.js"}}, root);
		const std::vector<std::string> expected{"before", "A", "a", "z", "low byte", "high byte", "after"};
		Check(ordered.size() == expected.size(), "discovery included an ignored entry or lost a source");
		for (std::size_t i = 0; i < ordered.size(); ++i)
		{
			Check(ordered[i].content == expected[i], "configured/bytewise ordering failed");
			Check(ordered[i].canonical_path.is_absolute(), "output path is not absolute");
			Check(
				ordered[i].canonical_path == fs::canonical(ordered[i].canonical_path), "output path is not canonical");
		}
		Check(ordered.front().canonical_path == fs::canonical(root / "before.js"),
			"relative source did not use config root");
		std::cout << "PASS: configured order, bytewise names, relative/absolute paths, ignored entries\n";

		for (const auto& source : ordered)
			Check(source.authority == Authority::User, "omitted authority must default to User");
		Check(Source{root / "before.js", "stub"}.authority == Authority::User,
			"two-field Source initialization must retain the User default");
		const auto authorities = LoadSources({{Kind::File, "before.js"}, {Kind::Directory, "plugins", Authority::Admin},
												 {Kind::File, "after.js", Authority::Admin}},
			root);
		Check(authorities.front().authority == Authority::User, "explicit User source gained authority");
		for (std::size_t i = 1; i < authorities.size(); ++i)
			Check(authorities[i].authority == Authority::Admin, "directory/file authority was not inherited");
		const auto grants = LoadSources({{Kind::File, "before.js", Authority::Admin, {"private"}},
											{Kind::Directory, "plugins", Authority::User, {"rss", "cache"}}},
			root);
		Check(grants.front().databases == std::set<std::string>{"private"}, "file database grants lost");
		for (std::size_t i = 1; i < grants.size(); ++i)
			Check(grants[i].databases == std::set<std::string>({"rss", "cache"}), "directory database grants lost");
		Check(ordered.front().databases.empty(), "omitted grants must deny databases");
		ExpectError("duplicate canonical path",
			[&]
			{
				LoadSources(
					{{Kind::File, "before.js", Authority::User}, {Kind::File, "before.js", Authority::Admin}}, root);
			});
		ExpectError("unknown script authority",
			[&] { LoadSources({{Kind::File, "before.js", static_cast<Authority>(999)}}, root); });
		std::cout << "PASS: User default, explicit/inherited Admin authority and cross-authority duplicates\n";

		fs::create_symlink(root / "before.js", root / "alias.js");
		fs::create_directory_symlink(directory, root / "plugins-link");
		const auto explicit_link = LoadSources({{Kind::File, "alias.js"}}, root);
		Check(explicit_link[0].content == "before" &&
				explicit_link[0].canonical_path == fs::canonical(root / "before.js"),
			"explicit file symlink did not resolve canonically");
		const auto directory_link = LoadSources({{Kind::Directory, "plugins-link"}}, root);
		Check(directory_link.size() == 5 && directory_link[0].canonical_path == directory / "A.js",
			"explicit directory symlink did not resolve canonically");
		Check(LoadSources({{Kind::File, "before.js"}}, root / "plugins-link" / "..")[0].content == "before",
			"config root did not canonicalize");
		ExpectError("cannot resolve", [&] { LoadSources({{Kind::File, "plugins/broken-link.js"}}, root); });
		std::cout << "PASS: explicit symlinks resolve; directory-entry symlinks are ignored\n";

		ExpectError("duplicate canonical path",
			[&] { LoadSources({{Kind::File, "before.js"}, {Kind::File, root / "./before.js"}}, root); });
		ExpectError("duplicate canonical path",
			[&] { LoadSources({{Kind::File, "before.js"}, {Kind::File, "alias.js"}}, root); });
		ExpectError("duplicate canonical path",
			[&] { LoadSources({{Kind::Directory, "plugins"}, {Kind::File, "plugins/a.js"}}, root); });
		ExpectError("duplicate canonical path",
			[&] { LoadSources({{Kind::Directory, "plugins"}, {Kind::Directory, "plugins-link"}}, root); });
		std::cout << "PASS: duplicate files, aliases, overlapping directories and explicit paths fail\n";

		ExpectError("cannot resolve", [&] { LoadSources({{Kind::File, "missing.js"}}, root); });
		ExpectError("cannot resolve", [&] { LoadSources({{Kind::Directory, "missing-dir"}}, root); });
		ExpectError("not a regular file", [&] { LoadSources({{Kind::File, "plugins/not-a-file.js"}}, root); });
		ExpectError("not a regular file", [&] { LoadSources({{Kind::File, "plugins/ignored-fifo.js"}}, root); });
		ExpectError("not an accessible directory", [&] { LoadSources({{Kind::Directory, "before.js"}}, root); });
		ExpectError(".js extension", [&] { LoadSources({{Kind::File, "plugins/ignored.txt"}}, root); });
		ExpectError(".js extension", [&] { LoadSources({{Kind::File, "plugins/ignored.JS"}}, root); });
		std::cout << "PASS: missing, nonregular, wrong-kind and non-.js sources fail without blocking\n";

		fs::create_directories(root / "empty");
		ExpectError("no regular .js files", [&] { LoadSources({{Kind::Directory, "empty"}}, root); });
		Write(root / "empty" / "only.txt", "ignored");
		fs::create_symlink(root / "before.js", root / "empty" / "only-link.js");
		ExpectError("no regular .js files", [&] { LoadSources({{Kind::Directory, "empty"}}, root); });
		Check(LoadSources({}, root).empty(), "empty configuration should yield an empty candidate");
		Write(root / "empty.js", "");
		Check(LoadSources({{Kind::File, "empty.js"}}, root)[0].content.empty(), "empty JS file should load");
		std::cout << "PASS: empty matching directories fail; empty specs and empty scripts are valid\n";

		Write(root / "four.js", "1234");
		Write(root / "three.js", "123");
		Check(LoadSources({{Kind::File, "four.js"}, {Kind::File, "three.js"}}, root, Limits{4, 7, 2}).size() == 2,
			"exact byte/count limits should pass");
		ExpectError("per-file byte limit", [&] { LoadSources({{Kind::File, "four.js"}}, root, Limits{3, 99, 9}); });
		ExpectError("total byte limit",
			[&] { LoadSources({{Kind::File, "four.js"}, {Kind::File, "three.js"}}, root, Limits{4, 6, 2}); });
		ExpectError("source count limit",
			[&] { LoadSources({{Kind::File, "four.js"}, {Kind::File, "three.js"}}, root, Limits{4, 7, 1}); });
		ExpectError(
			"source count limit", [&] { LoadSources({{Kind::Directory, "plugins"}}, root, Limits{99, 99, 4}); });
		ExpectError("all limits must be positive", [&] { LoadSources({}, root, Limits{0, 1, 1}); });
		ExpectError("all limits must be positive", [&] { LoadSources({}, root, Limits{1, 0, 1}); });
		ExpectError("all limits must be positive", [&] { LoadSources({}, root, Limits{1, 1, 0}); });
		std::cout << "PASS: per-file, total-byte, source-count bounds and exact-boundary cases\n";

		std::string bytes = "// CRLF\r\n";
		for (int repetition = 0; repetition < 65; ++repetition)
			for (unsigned int byte = 0; byte < 256; ++byte)
				bytes += static_cast<char>(byte);
		Write(root / "bytes.js", bytes);
		const auto preserved = LoadSources({{Kind::File, "bytes.js"}}, root);
		Check(preserved[0].content == bytes, "source bytes changed during reading");
		Write(root / "bytes.js", "changed after loading");
		Check(preserved[0].content == bytes, "loaded source does not own its bytes");
		std::cout << "PASS: binary bytes, NUL, CRLF, multibuffer reads and retained ownership\n";

		auto active = LoadSources({{Kind::File, "before.js"}}, root);
		ExpectError("cannot resolve",
			[&] { active = LoadSources({{Kind::File, "after.js"}, {Kind::File, "missing.js"}}, root); });
		Check(active.size() == 1 && active[0].content == "before" &&
				active[0].canonical_path == fs::canonical(root / "before.js"),
			"a failed candidate load changed active state");
		std::cout << "PASS: later-source failure leaves the caller's active set unchanged\n";

		ExpectError("paths must be nonempty", [&] { LoadSources({{Kind::File, ""}}, root); });
		ExpectError("paths must be nonempty", [&] { LoadSources({}, ""); });
		ExpectError("no NUL bytes",
			[&] { LoadSources({{Kind::File, fs::path(std::string("before.js\0hidden.js", 19))}}, root); });
		ExpectError("config root", [&] { LoadSources({}, root / "before.js"); });
		ExpectError("unknown source kind", [&] { LoadSources({{static_cast<Kind>(999), "before.js"}}, root); });
		std::cout << "PASS: malformed paths, config roots and source kinds fail clearly\n";

		const std::string wasm_header("\0asm\x01\0\0\0", 8);
		std::string wasm_bytes;
		for (unsigned int byte = 0; byte < 256; ++byte)
			wasm_bytes += static_cast<char>(byte);
		Write(root / "core.wasm", wasm_header);
		Write(root / "bytes.wasm", wasm_bytes);
		const auto assets =
			LoadWasmAssets({{"core_v1-1", "core.wasm"}, {"Bytes", root / "bytes.wasm"}}, root, Limits{256, 264, 2});
		Check(assets.size() == 2 && assets[0].name == "core_v1-1" && assets[1].name == "Bytes",
			"Wasm asset order or names changed");
		Check(assets[0].canonical_path == fs::canonical(root / "core.wasm") &&
				assets[1].canonical_path == fs::canonical(root / "bytes.wasm"),
			"Wasm paths are not canonical");
		Check(assets[0].content == wasm_header && assets[1].content == wasm_bytes,
			"Wasm binary bytes were not preserved");
		Write(root / "bytes.wasm", "changed");
		Check(assets[1].content == wasm_bytes, "Wasm assets do not own their bytes");
		Write(root / "bytes.wasm", wasm_bytes);
		Check(LoadWasmAssets({}, root).empty(), "empty Wasm configuration should be valid");
		Check(LoadWasmAssets({{std::string(64, 'x'), "core.wasm"}}, root).size() == 1,
			"64-byte Wasm name should be valid");
		Check(LoadWasmAssets({{"Core", "core.wasm"}, {"core", "bytes.wasm"}}, root).size() == 2,
			"Wasm names must be case-sensitive");
		std::cout << "PASS: owned binary Wasm assets, canonical paths, names, order and exact limits\n";

		for (const auto& invalid : std::vector<std::string>{"", "has.dot", "has/slash", "has space", "line\nbreak",
				 std::string(1, '\0'), std::string(1, '\x80'), std::string(65, 'x')})
			ExpectError("Wasm name must be", [&] { LoadWasmAssets({{invalid, "core.wasm"}}, root); });
		ExpectError(
			"duplicate Wasm name", [&] { LoadWasmAssets({{"core", "core.wasm"}, {"core", "bytes.wasm"}}, root); });
		ExpectError("duplicate canonical path",
			[&] { LoadWasmAssets({{"core", "core.wasm"}, {"other", root / "./core.wasm"}}, root); });
		fs::create_symlink(root / "core.wasm", root / "alias.wasm");
		Check(LoadWasmAssets({{"alias", "alias.wasm"}}, root)[0].canonical_path == fs::canonical(root / "core.wasm"),
			"explicit Wasm symlink should resolve canonically");
		ExpectError("duplicate canonical path",
			[&] { LoadWasmAssets({{"core", "core.wasm"}, {"alias", "alias.wasm"}}, root); });
		ExpectError("cannot resolve", [&] { LoadWasmAssets({{"missing", "missing.wasm"}}, root); });
		ExpectError(".wasm extension", [&] { LoadWasmAssets({{"wrong", "before.js"}}, root); });
		ExpectError(".wasm extension", [&] { LoadWasmAssets({{"wrong", "core.WASM"}}, root); });
		ExpectError("paths must be nonempty", [&] { LoadWasmAssets({{"empty", ""}}, root); });
		fs::create_directory(root / "directory.wasm");
		ExpectError("not a regular file", [&] { LoadWasmAssets({{"directory", "directory.wasm"}}, root); });
		ExpectError("per-file byte limit", [&] { LoadWasmAssets({{"core", "core.wasm"}}, root, Limits{7, 99, 2}); });
		ExpectError("total byte limit",
			[&] { LoadWasmAssets({{"core", "core.wasm"}, {"bytes", "bytes.wasm"}}, root, Limits{256, 263, 2}); });
		ExpectError("source count limit",
			[&] { LoadWasmAssets({{"core", "core.wasm"}, {"bytes", "bytes.wasm"}}, root, Limits{256, 264, 1}); });
		auto active_assets = assets;
		ExpectError("cannot resolve",
			[&] { active_assets = LoadWasmAssets({{"core", "core.wasm"}, {"missing", "missing.wasm"}}, root); });
		Check(
			active_assets.size() == 2 && active_assets[0].name == "core_v1-1" && active_assets[1].content == wasm_bytes,
			"failed Wasm candidate changed active assets");
		std::cout << "PASS: Wasm invalid names, duplicate names/paths, path errors, bounds and failure isolation\n";

		if (::geteuid() != 0)
		{
			Write(root / "unreadable.js", "unreadable fixture");
			fs::permissions(root / "unreadable.js", fs::perms::none);
			ExpectError("cannot open", [&] { LoadSources({{Kind::File, "unreadable.js"}}, root); });
			fs::permissions(root / "unreadable.js", fs::perms::owner_all);
			fs::create_directory(root / "unreadable-directory");
			fs::permissions(root / "unreadable-directory", fs::perms::none);
			ExpectError("cannot enumerate", [&] { LoadSources({{Kind::Directory, "unreadable-directory"}}, root); });
			fs::permissions(root / "unreadable-directory", fs::perms::owner_all);
			Write(root / "unreadable.wasm", wasm_header);
			fs::permissions(root / "unreadable.wasm", fs::perms::none);
			ExpectError("cannot open", [&] { LoadWasmAssets({{"unreadable", "unreadable.wasm"}}, root); });
			fs::permissions(root / "unreadable.wasm", fs::perms::owner_all);
			std::cout << "PASS: unreadable file and directory errors\n";
		}
		else
			std::cout << "SKIP: mode-bit permission tests require a non-root user\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
