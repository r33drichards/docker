// Synthetic credentials only. Included by the existing transport test binary.
#include <filesystem>
#include <fstream>

static void HeaderTests(const std::string& directory)
{
	namespace fs = std::filesystem;
	const auto file = directory + "/secret";
	const auto write = [&](const std::string& value, mode_t mode = 0600)
	{
		::chmod(file.c_str(), 0600);
		std::ofstream out(file, std::ios::binary | std::ios::trunc);
		out.write(value.data(), value.size());
		out.close();
		assert(out);
		assert(::chmod(file.c_str(), mode) == 0);
	};
	const auto rejects = [](auto operation)
	{
		bool rejected = false;
		try
		{
			operation();
		}
		catch (const std::runtime_error& error)
		{
			rejected = true;
			assert(std::string(error.what()).find("synthetic") == std::string::npos);
		}
		assert(rejected);
	};
	for (const auto& name : {"", "bad name", "bad:field", "bad\r\nfield", "Host", "CONTENT-TYPE", "Content-Length",
			 "Content-Encoding", "Transfer-Encoding", "Accept-Encoding", "Expect", "Connection", "Keep-Alive", "TE",
			 "Trailer", "Upgrade", "HTTP2-Settings", "Proxy-Authorization", "pRoXy-test", "Cookie", "Set-Cookie",
			 "Method-Override", "X-Method-Override", "X-HTTP-Method", "X-HTTP-Method-Override"})
		rejects([&] { HeaderName(name); });
	rejects([&] { HeaderName(std::string(65, 'a')); });
	assert(HeaderName("XC-Token") == "xc-token");
	assert(HeaderName("!#$%&'*+-.^_`|~") == "!#$%&'*+-.^_`|~");
	write("synthetic-token", 0400);
	assert(ReadHeaderSecret(file) == "synthetic-token");
	write("Bearer synthetic-token");
	Endpoint endpoint;
	AddHeader(endpoint, "Authorization", file);
	const auto original = endpoint.headers;
	AddHeader(endpoint, "xc-token", file);
	assert(original->size() == 1 && endpoint.headers->size() == 2);
	rejects([&] { AddHeader(endpoint, "AUTHORIZATION", file); });
	assert(endpoint.headers->size() == 2);
	for (int i = 0; i < 6; ++i)
		AddHeader(endpoint, "x-test-" + std::to_string(i), file);
	rejects([&] { AddHeader(endpoint, "x-nine", file); });
	write(std::string(MaxHeaderValue, 'a'));
	Endpoint aggregate;
	for (int i = 0; i < 7; ++i)
		AddHeader(aggregate, "x-" + std::to_string(i), file);
	rejects([&] { AddHeader(aggregate, "x-eight", file); });
	assert(aggregate.headers->size() == 7);
	for (int byte = 0; byte < 32; ++byte)
	{
		write("synthetic" + std::string(1, static_cast<char>(byte)));
		rejects([&] { ReadHeaderSecret(file); });
	}
	for (const auto& value : {std::string(), std::string(MaxHeaderValue + 1, 'a'), std::string("synthetic\177")})
	{
		write(value);
		rejects([&] { ReadHeaderSecret(file); });
	}
	for (mode_t mode : {0644, 0640, 0660, 0700, 0000, 04600})
	{
		write("synthetic", mode);
		rejects([&] { ReadHeaderSecret(file); });
	}
	write("synthetic-token");
	struct stat ownedfile{};
	ownedfile.st_mode = S_IFREG | 0600;
	ownedfile.st_uid = ::geteuid();
	ownedfile.st_size = 10;
	assert(SafeSecretFile(ownedfile));
	ownedfile.st_uid = ::geteuid() + 1;
	assert(!SafeSecretFile(ownedfile));
	struct stat parent{};
	parent.st_mode = S_IFDIR | 0700;
	parent.st_uid = ::geteuid();
	assert(SafeSecretDirectory(parent, true));
	parent.st_uid = ::geteuid() + 1;
	assert(!SafeSecretDirectory(parent, false));
	parent.st_uid = ::geteuid();
	parent.st_mode = S_IFDIR | 0770;
	assert(!SafeSecretDirectory(parent, false));
	fs::create_directory(directory + "/unsafe-parent");
	fs::copy_file(file, directory + "/unsafe-parent/secret");
	assert(::chmod((directory + "/unsafe-parent").c_str(), 0770) == 0);
	rejects([&] { ReadHeaderSecret(directory + "/unsafe-parent/secret"); });
	fs::create_symlink(file, directory + "/link");
	fs::create_directory_symlink(directory, directory + "/parent-link");
	assert(::mkfifo((directory + "/fifo").c_str(), 0600) == 0);
	for (const auto& path : {directory + "/link", directory + "/parent-link/secret", directory + "/fifo", directory,
			 directory + "/missing", directory + "/../secret", directory + "/./secret", directory + "//secret",
			 std::string("relative"), file + std::string(1, '\0') + "suffix"})
		rejects([&] { ReadHeaderSecret(path); });
	const auto retained = original->at("authorization");
	assert(retained == "Bearer synthetic-token");
	std::cout << "PASS: header names, reserved fields, secret file safety, controls, limits, immutable snapshots\n";
}
