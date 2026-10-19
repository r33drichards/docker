/* SPDX-License-Identifier: MIT */
#include "input.h"
#include "../../modules/js_script_loader.h"

#include <fstream>
#include <optional>

namespace
{
namespace fs = std::filesystem;
using Authority = js_scripts::Authority;
using Kind = js_scripts::SourceSpec::Kind;

// All filesystem paths are fixed by this harness. Fuzz bytes are only file
// contents, limits, enum values and Wasm logical names, never an OS path.
class Fixture
{
public:
	fs::path root;
	Fixture()
	{
		char path[] = "/tmp/irc-js-fuzz-loader-XXXXXX";
		auto* created = ::mkdtemp(path);
		fuzz_test::Require(created != nullptr, "mkdtemp failed");
		root = created;
		fs::create_directory(root / "scripts");
		fs::create_directory(root / "empty");
		fs::create_directory(root / "directory.js");
		fs::create_directory(root / "scripts" / "nested.js");
		fs::create_symlink(root / "a.js", root / "alias.js");
		fs::create_symlink(root / "a.js", root / "scripts" / "ignored.js");
		fs::create_symlink(root / "asset.wasm", root / "alias.wasm");
		fuzz_test::Require(::mkfifo((root / "fifo.js").c_str(), 0600) == 0, "mkfifo failed");
		Write("ignored.txt", "ignored");
		Write("scripts/ignored.txt", "ignored");
	}
	~Fixture()
	{
		std::error_code error;
		fs::remove_all(root, error);
	}
	void Write(const fs::path& relative, const std::string& bytes) const
	{
		std::ofstream stream(root / relative, std::ios::binary | std::ios::trunc);
		stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		stream.close();
		fuzz_test::Require(!stream.fail(), "fixture write failed");
	}
};

bool WithinLimits(const std::vector<std::string>& contents, const js_scripts::Limits& limits)
{
	if (!limits.max_file_bytes || !limits.max_total_bytes || !limits.max_sources ||
		contents.size() > limits.max_sources)
		return false;
	std::size_t total = 0;
	for (const auto& content : contents)
	{
		if (content.size() > limits.max_file_bytes)
			return false;
		total += content.size(); // At most two 384-byte harness files.
	}
	return total <= limits.max_total_bytes;
}

void CheckSources(const Fixture& fixture, unsigned scenario, unsigned authority_byte, const std::string& first,
	const std::string& second, const js_scripts::Limits& limits)
{
	const auto authority = authority_byte % 3 == 0 ? Authority::User
		: authority_byte % 3 == 1				   ? Authority::Admin
												   : static_cast<Authority>(37);
	std::vector<js_scripts::SourceSpec> specs;
	std::vector<fs::path> expected_paths;
	std::vector<std::string> expected_contents;
	bool valid = authority == Authority::User || authority == Authority::Admin;
	const auto file = [&](const fs::path& configured, const fs::path& canonical, const std::string& content)
	{
		specs.push_back({Kind::File, configured, authority});
		expected_paths.push_back(fixture.root / canonical);
		expected_contents.push_back(content);
	};
	switch (scenario % 12)
	{
	case 0:
		file("a.js", "a.js", first);
		break;
	case 1:
		specs.push_back({Kind::Directory, "scripts", authority});
		expected_paths = {fixture.root / "scripts/A.js", fixture.root / "scripts/\x80.js"};
		expected_contents = {first, second};
		break;
	case 2:
		file("a.js", "a.js", first);
		file("alias.js", "a.js", first);
		valid = false;
		break;
	case 3:
		file("alias.js", "a.js", first);
		break;
	case 4:
		file("missing.js", "missing.js", {});
		valid = false;
		break;
	case 5:
		file("ignored.txt", "ignored.txt", "ignored");
		valid = false;
		break;
	case 6:
		file("fifo.js", "fifo.js", {});
		valid = false;
		break;
	case 7:
		file("directory.js", "directory.js", {});
		valid = false;
		break;
	case 8:
		file(std::string("a.js\0suffix", 11), "a.js", first);
		valid = false;
		break;
	case 9:
		specs.push_back({static_cast<Kind>(37), "a.js", authority});
		valid = false;
		break;
	case 10:
		valid = true;
		break; // Empty sets ignore unused authority.
	case 11:
		specs.push_back({Kind::Directory, "empty", authority});
		valid = false;
		break;
	}
	const bool expected_success = valid && WithinLimits(expected_contents, limits);
	std::optional<std::vector<js_scripts::Source>> actual;
	try
	{
		actual = js_scripts::LoadSources(specs, fixture.root, limits);
	}
	catch (const js_scripts::LoadError&)
	{
	}
	fuzz_test::Require(actual.has_value() == expected_success, "source accept/reject differs from model");
	if (!actual)
		return;
	fuzz_test::Require(actual->size() == expected_paths.size(), "source count");
	for (std::size_t index = 0; index < actual->size(); ++index)
	{
		fuzz_test::Require((*actual)[index].canonical_path == expected_paths[index], "source path/order");
		fuzz_test::Require((*actual)[index].content == expected_contents[index], "source bytes");
		fuzz_test::Require((*actual)[index].authority == authority, "source authority");
	}
}

void CheckWasm(const Fixture& fixture, unsigned scenario, const std::string& name, const std::string& content,
	const js_scripts::Limits& limits)
{
	// Explicit ASCII validation is intentionally different from production's
	// all_of predicate, and binary payloads need not be valid Wasm modules.
	bool valid = !name.empty() && name.size() <= 64;
	const std::string allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";
	for (char byte : name)
		valid = valid && allowed.find(byte) != std::string::npos;
	std::vector<js_scripts::WasmSpec> specs;
	std::vector<std::string> contents;
	switch (scenario % 7)
	{
	case 0:
		specs.push_back({name, "asset.wasm"});
		contents.push_back(content);
		break;
	case 1:
		specs.push_back({name, "alias.wasm"});
		contents.push_back(content);
		break;
	case 2:
		specs = {{name, "asset.wasm"}, {name, "alias.wasm"}};
		valid = false;
		break;
	case 3:
		specs = {{name, "asset.wasm"}, {name + "_", "alias.wasm"}};
		valid = false;
		break;
	case 4:
		specs.push_back({name, "a.js"});
		valid = false;
		break;
	case 5:
		specs.push_back({name, "missing.wasm"});
		valid = false;
		break;
	case 6:
		valid = true;
		break;
	}
	const bool expected_success = valid && WithinLimits(contents, limits);
	std::optional<std::vector<js_scripts::WasmAsset>> actual;
	try
	{
		actual = js_scripts::LoadWasmAssets(specs, fixture.root, limits);
	}
	catch (const js_scripts::LoadError&)
	{
	}
	fuzz_test::Require(actual.has_value() == expected_success, "Wasm accept/reject differs from model");
	if (!actual)
		return;
	fuzz_test::Require(actual->size() == contents.size(), "Wasm count");
	for (const auto& asset : *actual)
	{
		fuzz_test::Require(asset.canonical_path == fixture.root / "asset.wasm", "Wasm canonical path");
		fuzz_test::Require(asset.content == content, "Wasm bytes");
		fuzz_test::Require(asset.name == name, "Wasm name");
	}
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
	fuzz_test::Input input(data, size);
	const auto source_case = input.Byte();
	const auto authority = input.Byte();
	const auto wasm_case = input.Byte();
	const js_scripts::Limits limits{input.Word() % 513, input.Word() % 1025, input.Byte() % 5};
	const auto first = input.Take(input.Word() % 385);
	const auto second = input.Take(input.Word() % 385);
	const auto name = input.Take(input.Byte() % 67);
	static const Fixture fixture;
	fixture.Write("a.js", first);
	fixture.Write("scripts/A.js", first);
	fixture.Write("scripts/\x80.js", second);
	fixture.Write("asset.wasm", second);
	CheckSources(fixture, source_case, authority, first, second, limits);
	CheckWasm(fixture, wasm_case, name, second, limits);
	return 0;
}
