/* SPDX-License-Identifier: MIT */
#pragma once

// Owned, bounded webhook jobs. The worker never enters V8 or touches IRC state.
#include <curl/curl.h>
#include <atomic>
#include <condition_variable>
#include <array>
#include <cerrno>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace js_webhooks
{
constexpr std::size_t MaxPending = 64;
constexpr std::size_t MaxBody = 16384;
constexpr std::size_t MaxResponse = 65536;

inline void EnsureCurl()
{
	// Process lifetime, like the embedding V8 platform. No other module's curl
	// users are invalidated by rehash/module unload via curl_global_cleanup.
	static const bool ready = []
	{
		if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
			return false;
		const auto* version = curl_version_info(CURLVERSION_NOW);
		return version && version->version_num >= 0x075500 && (version->features & CURL_VERSION_SSL) &&
			(version->features & CURL_VERSION_ASYNCHDNS) && (version->features & CURL_VERSION_THREADSAFE);
	}();
	if (!ready)
		throw std::runtime_error(
			"Webhook transport requires curl 7.85+ with TLS, thread-safe initialization and asynchronous DNS");
}

inline bool ValidName(const std::string& name)
{
	if (name.empty() || name.size() > 64)
		return false;
	for (const unsigned char c : name)
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
			return false;
	return true;
}

constexpr std::size_t MaxHeaders = 8;
constexpr std::size_t MaxHeaderName = 64;
constexpr std::size_t MaxHeaderValue = 1024;
constexpr std::size_t MaxHeaderBytes = 8192;

inline std::string HeaderName(const std::string& name)
{
	if (name.empty() || name.size() > MaxHeaderName)
		throw std::runtime_error("Invalid webhook header name");
	std::string lower;
	for (unsigned char c : name)
	{
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
				std::string("!#$%&'*+-.^_`|~").find(c) != std::string::npos))
			throw std::runtime_error("Invalid webhook header name");
		lower += (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
	}
	for (const char* denied : {"host", "content-type", "content-length", "content-encoding", "transfer-encoding",
			 "accept-encoding", "expect", "connection", "keep-alive", "te", "trailer", "upgrade", "http2-settings",
			 "cookie", "set-cookie", "method-override", "x-method-override", "x-http-method", "x-http-method-override"})
		if (lower == denied)
			throw std::runtime_error("Reserved webhook header name");
	if (lower.compare(0, 6, "proxy-") == 0)
		throw std::runtime_error("Reserved webhook header name");
	return lower;
}

inline bool SafeSecretFile(const struct stat& info)
{
	return S_ISREG(info.st_mode) && info.st_uid == ::geteuid() &&
		((info.st_mode & 07777) == 0400 || (info.st_mode & 07777) == 0600) && info.st_size > 0 &&
		info.st_size <= static_cast<off_t>(MaxHeaderValue);
}

inline bool SafeSecretDirectory(const struct stat& info, bool requireprivate)
{
	const bool owned = info.st_uid == ::geteuid();
	if (!S_ISDIR(info.st_mode) || (!owned && info.st_uid != 0))
		return false;
	if (requireprivate)
		return owned && (info.st_mode & 07777) == 0700;
	// A root-owned sticky shared ancestor (e.g. /tmp) is only accepted when
	// followed immediately by a private daemon-owned directory.
	return !(info.st_mode & 0022) || (info.st_uid == 0 && (info.st_mode & S_ISVTX));
}

// Linux descriptor-relative traversal: no symlinks, including parent components.
// Error text deliberately includes neither a path nor file contents.
inline std::string ReadHeaderSecret(const std::string& path)
{
	const auto fail = [] { throw std::runtime_error("Cannot load webhook header secret file"); };
	if (path.empty() || path[0] != '/' || path.size() > 4096 || path.find('\0') != std::string::npos)
		fail();
	struct Descriptor
	{
		int fd;
		~Descriptor()
		{
			if (fd >= 0)
				::close(fd);
		}
	} current{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
	if (current.fd < 0)
		fail();
	bool requireprivate = false;
	std::size_t start = 1;
	for (;;)
	{
		struct stat parent{};
		if (::fstat(current.fd, &parent) || !SafeSecretDirectory(parent, requireprivate))
			fail();
		requireprivate = (parent.st_mode & 0022) != 0;
		const auto end = path.find('/', start);
		if (requireprivate && end == std::string::npos)
			fail();
		const auto part = path.substr(start, end == std::string::npos ? end : end - start);
		if (part.empty() || part == "." || part == "..")
			fail();
		int next;
		do
		{
			next = ::openat(current.fd, part.c_str(),
				O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (end == std::string::npos ? 0 : O_DIRECTORY));
		} while (next < 0 && errno == EINTR);
		if (next < 0)
			fail();
		::close(current.fd);
		current.fd = next;
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	struct stat before{}, after{};
	if (::fstat(current.fd, &before) || !SafeSecretFile(before))
		fail();
	std::array<char, MaxHeaderValue + 1> bytes{};
	std::size_t count = 0;
	for (;;)
	{
		const auto got = ::read(current.fd, bytes.data() + count, bytes.size() - count);
		if (got < 0)
		{
			if (errno == EINTR)
				continue;
			fail();
		}
		if (!got)
			break;
		count += static_cast<std::size_t>(got);
		if (count > MaxHeaderValue)
			fail();
	}
	if (::fstat(current.fd, &after) || before.st_size != static_cast<off_t>(count) || before.st_size != after.st_size ||
		before.st_mode != after.st_mode || before.st_uid != after.st_uid ||
		before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
		before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
		fail();
	for (std::size_t i = 0; i < count; ++i)
		if (static_cast<unsigned char>(bytes[i]) < 32 || static_cast<unsigned char>(bytes[i]) == 127)
			fail();
	return std::string(bytes.data(), count);
}

using Headers = std::map<std::string, std::string>;

struct Endpoint
{
	std::string url;
	bool loopbackhttp = false;
	std::shared_ptr<const Headers> headers = std::make_shared<const Headers>();
};

inline void AddHeader(Endpoint& endpoint, const std::string& name, const std::string& valuefile)
{
	const auto normalized = HeaderName(name);
	if (endpoint.headers->size() >= MaxHeaders || endpoint.headers->count(normalized))
		throw std::runtime_error("Duplicate or excessive webhook headers");
	auto value = ReadHeaderSecret(valuefile);
	std::size_t bytes = normalized.size() + value.size() + 4;
	for (const auto& header : *endpoint.headers)
		bytes += header.first.size() + header.second.size() + 4;
	if (bytes > MaxHeaderBytes)
		throw std::runtime_error("Excessive webhook header bytes");
	auto candidate = std::make_shared<Headers>(*endpoint.headers);
	candidate->emplace(normalized, std::move(value));
	endpoint.headers = std::move(candidate);
}

inline Endpoint ValidateEndpoint(const std::string& url, bool loopbackhttp = false)
{
	EnsureCurl();
	if (url.empty() || url.size() > 2048)
		throw std::runtime_error("Invalid webhook URL");
	for (unsigned char c : url)
		if (c <= 32 || c == 127)
			throw std::runtime_error("Invalid webhook URL");
	std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
	if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), 0) != CURLUE_OK)
		throw std::runtime_error("Invalid webhook URL");
	auto part = [&](CURLUPart field)
	{
		char* value = nullptr;
		const auto status = curl_url_get(parsed.get(), field, &value, 0);
		std::unique_ptr<char, decltype(&curl_free)> owned(value, curl_free);
		return std::make_pair(status == CURLUE_OK, value ? std::string(value) : std::string());
	};
	const auto scheme = part(CURLUPART_SCHEME);
	const auto host = part(CURLUPART_HOST);
	if (part(CURLUPART_USER).first || part(CURLUPART_PASSWORD).first || part(CURLUPART_FRAGMENT).first || !host.first ||
		(scheme.second != "https" &&
			!(loopbackhttp && scheme.second == "http" && (host.second == "127.0.0.1" || host.second == "[::1]"))))
		throw std::runtime_error(
			"Webhooks require HTTPS without userinfo or fragments; HTTP is explicit loopback only");
	return {url, scheme.second == "http"};
}

struct Generation
{
	const std::map<std::string, Endpoint> endpoints;
	std::atomic<bool> alive{false};
	explicit Generation(std::map<std::string, Endpoint> values)
		: endpoints(std::move(values))
	{
	}
};

struct Counters
{
	std::atomic<std::size_t> pending{0};
	std::atomic<std::size_t> sent{0};
	std::atomic<std::size_t> failed{0};
	std::atomic<std::size_t> discarded{0};
};

class Request
{
	const std::shared_ptr<Counters> counts;

public:
	const std::shared_ptr<Generation> generation;
	const Endpoint endpoint;
	const std::string body;
	Request(std::shared_ptr<Counters> stats, std::shared_ptr<Generation> owner, Endpoint destination, std::string json)
		: counts(std::move(stats))
		, generation(std::move(owner))
		, endpoint(std::move(destination))
		, body(std::move(json))
	{
	}
	~Request() { --counts->pending; }
	Request(const Request&) = delete;
	Request& operator=(const Request&) = delete;
};

class Dispatcher
{
	std::shared_ptr<Counters> counts = std::make_shared<Counters>();
	std::atomic<bool> stopping{false};
	std::mutex mutex;
	std::condition_variable wake;
	std::deque<std::shared_ptr<Request>> queue;
	std::thread worker;

	struct Transfer
	{
		const Dispatcher& owner;
		const Request& request;
		std::size_t received = 0;
		bool Cancelled() const { return owner.stopping || !request.generation->alive; }
	};
	static int Progress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
	{
		return static_cast<Transfer*>(opaque)->Cancelled() ? 1 : 0;
	}
	static std::size_t Discard(char*, std::size_t size, std::size_t count, void* opaque)
	{
		auto& transfer = *static_cast<Transfer*>(opaque);
		if (transfer.Cancelled() || (size && count > (MaxResponse - transfer.received) / size))
			return 0;
		const auto bytes = size * count;
		transfer.received += bytes;
		return bytes;
	}
	bool Send(const Request& request)
	{
		std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
		if (!curl)
			return false;
		curl_slist* rawheaders = nullptr;
		for (const char* header : {"Content-Type: application/json", "Expect:"})
		{
			auto* next = curl_slist_append(rawheaders, header);
			if (!next)
			{
				curl_slist_free_all(rawheaders);
				return false;
			}
			rawheaders = next;
		}
		std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(rawheaders, curl_slist_free_all);
		for (const auto& header : *request.endpoint.headers)
		{
			const auto line = header.first + ": " + header.second;
			auto* next = curl_slist_append(headers.get(), line.c_str());
			if (!next)
				return false;
			(void)headers.release();
			headers.reset(next);
		}
		Transfer transfer{*this, request};
		// Every option must be accepted: silently dropping a security option is
		// not a supported fallback. No environment proxy, redirects or retries.
		auto set = [&](CURLoption option, auto value)
		{ return curl_easy_setopt(curl.get(), option, value) == CURLE_OK; };
		if (!set(CURLOPT_URL, request.endpoint.url.c_str()) ||
			!set(CURLOPT_PROTOCOLS_STR, request.endpoint.loopbackhttp ? "http" : "https") ||
			!set(CURLOPT_FOLLOWLOCATION, 0L) || !set(CURLOPT_PROXY, "") || !set(CURLOPT_SSL_VERIFYPEER, 1L) ||
			!set(CURLOPT_SSL_VERIFYHOST, 2L) || !set(CURLOPT_NOSIGNAL, 1L) || !set(CURLOPT_CONNECTTIMEOUT_MS, 1000L) ||
			!set(CURLOPT_TIMEOUT_MS, 2000L) || !set(CURLOPT_HTTPHEADER, headers.get()) || !set(CURLOPT_POST, 1L) ||
			!set(CURLOPT_POSTFIELDS, request.body.data()) ||
			!set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size())) ||
			!set(CURLOPT_WRITEFUNCTION, &Discard) || !set(CURLOPT_WRITEDATA, &transfer) ||
			!set(CURLOPT_HEADERFUNCTION, &Discard) || !set(CURLOPT_HEADERDATA, &transfer) ||
			!set(CURLOPT_XFERINFOFUNCTION, &Progress) || !set(CURLOPT_XFERINFODATA, &transfer) ||
			!set(CURLOPT_NOPROGRESS, 0L))
			return false;
		if (transfer.Cancelled() || curl_easy_perform(curl.get()) != CURLE_OK)
			return false;
		long status = 0;
		return curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status) == CURLE_OK && status >= 200 &&
			status < 300;
	}
	void Run() noexcept
	{
		for (;;)
		{
			std::shared_ptr<Request> request;
			{
				std::unique_lock<std::mutex> lock(mutex);
				wake.wait(lock, [&] { return stopping || !queue.empty(); });
				if (stopping)
					return;
				request = std::move(queue.front());
				queue.pop_front();
			}
			if (!request->generation->alive)
			{
				++counts->discarded;
				continue;
			}
			bool sent = false;
			try
			{
				sent = Send(*request);
			}
			catch (...)
			{ /* Never throw out of the worker. */
			}
			if (sent)
				++counts->sent;
			else
				++counts->failed;
		}
	}

public:
	Dispatcher() = default;
	~Dispatcher() { Stop(); }
	Dispatcher(const Dispatcher&) = delete;
	Dispatcher& operator=(const Dispatcher&) = delete;
	void Start()
	{
		if (!worker.joinable())
		{
			EnsureCurl();
			worker = std::thread([this] { Run(); });
		}
	}
	// One owner/main thread calls Stage, Commit, Start, Stop. Worker receives
	// owned requests only. The reservation covers staged, queued AND in-flight
	// payloads, not merely the deque. Aborted effects release it automatically.
	std::shared_ptr<Request> Stage(
		const std::shared_ptr<Generation>& generation, const std::string& name, const std::string& body)
	{
		if (!generation || !generation->alive || stopping || body.size() > MaxBody)
			return nullptr;
		const auto found = generation->endpoints.find(name);
		if (found == generation->endpoints.end() || counts->pending >= MaxPending)
			return nullptr;
		++counts->pending;
		try
		{
			return std::make_shared<Request>(counts, generation, found->second, body);
		}
		catch (...)
		{
			--counts->pending;
			throw;
		}
	}
	void Commit(std::shared_ptr<Request> request)
	{
		if (!request)
			return;
		std::lock_guard<std::mutex> lock(mutex);
		if (stopping || !request->generation->alive)
		{
			++counts->discarded;
			return;
		}
		queue.push_back(std::move(request));
		wake.notify_one();
	}
	const Counters& Stats() const { return *counts; }
	void Stop()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
			counts->discarded += queue.size();
			queue.clear();
		}
		wake.notify_one();
		if (worker.joinable())
			worker.join();
	}
};
} // namespace js_webhooks
