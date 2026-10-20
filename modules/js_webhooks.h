/* SPDX-License-Identifier: MIT */
#pragma once

// Legacy named-webhook adapter over the canonical owned HTTP transport.
#include "js_async_http.h"
#include <curl/curl.h>
#include <atomic>
#include <array>
#include <cerrno>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstddef>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
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
// Exact destination identity. No wildcard, prefix or URL substring matching.
// Preserve path/query bytes and distinguish absent query from an empty query.
inline std::string ExactUrl(const std::string& url)
{
	EnsureCurl();
	if (url.empty() || url.size() > 2048 || url.find('\0') != std::string::npos ||
		url.find_first_of("\r\n\t \\#") != std::string::npos)
		return {};
	std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
	if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), CURLU_PATH_AS_IS) != CURLUE_OK)
		return {};
	auto part = [&](CURLUPart field)
	{
		char* raw = nullptr;
		auto status = curl_url_get(parsed.get(), field, &raw, 0);
		std::string value = raw ? raw : "";
		curl_free(raw);
		return std::make_pair(status == CURLUE_OK, value);
	};
	auto scheme = part(CURLUPART_SCHEME).second, host = part(CURLUPART_HOST).second, port = part(CURLUPART_PORT).second;
	for (auto* value : {&scheme, &host})
		for (char& c : *value)
			if (c >= 'A' && c <= 'Z')
				c += ('a' - 'A');
	if ((scheme != "http" && scheme != "https") || host.empty() || part(CURLUPART_USER).first ||
		part(CURLUPART_PASSWORD).first || part(CURLUPART_FRAGMENT).first)
		return {};
	if (port.empty())
		port = scheme == "https" ? "443" : "80";
	auto path = part(CURLUPART_PATH).second;
	if (path.empty())
		path = "/";
	// Do not let curl's dot-segment normalization silently change a matched path.
	std::size_t start = 0;
	while (start <= path.size())
	{
		auto end = path.find('/', start);
		auto segment = path.substr(start, end == std::string::npos ? end : end - start);
		if (segment == "." || segment == "..")
			return {};
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	const auto query = part(CURLUPART_QUERY);
	return scheme + "://" + host + ":" + port + path +
		((query.first || url.find('?') != std::string::npos) ? "?" + query.second : "");
}
using HeaderRules = std::map<std::pair<std::string, std::string>, Endpoint>;

struct Generation
{
	const std::map<std::string, Endpoint> endpoints;
	const HeaderRules headers;
	std::shared_ptr<js_async_http::Generation> http = std::make_shared<js_async_http::Generation>();
	std::atomic<bool>& alive = http->active;
	explicit Generation(std::map<std::string, Endpoint> values, HeaderRules rules = {})
		: endpoints(std::move(values))
		, headers(std::move(rules))
	{
		alive = false;
	}
	Headers ForUrl(const std::string& script, const std::string& url) const
	{
		const auto key = ExactUrl(url);
		if (key.empty())
			return {};
		const auto found = headers.find({script, key});
		return found == headers.end() ? Headers{} : *found->second.headers;
	}
	~Generation() { alive = false; }
};
struct Counters
{
	std::atomic<std::size_t> pending{0}, sent{0}, failed{0}, discarded{0};
};
struct Reservation
{
	std::shared_ptr<Counters> counts;
	bool completed = false, committed = false;
	explicit Reservation(std::shared_ptr<Counters> value)
		: counts(std::move(value))
	{
		++counts->pending;
	}
	~Reservation()
	{
		--counts->pending;
		if (committed && !completed)
			++counts->discarded;
	}
};
class Request
{
public:
	std::shared_ptr<Generation> generation;
	std::shared_ptr<js_async_http::Generation> scriptgeneration;
	std::shared_ptr<js_async_http::Dispatcher> transport;
	std::string id;
	std::shared_ptr<Reservation> reservation;
	bool committed = false;
	Request(std::shared_ptr<Generation> owner, std::shared_ptr<js_async_http::Dispatcher> dispatcher, std::string token)
		: generation(std::move(owner))
		, transport(std::move(dispatcher))
		, id(std::move(token))
	{
	}
	void Commit()
	{
		if (!generation->alive)
			return;
		reservation->committed = true;
		committed = transport->Commit(id);
	}
	~Request()
	{
		if (!committed)
			transport->Cancel(scriptgeneration, id);
	}
};
class Dispatcher
{
	std::shared_ptr<Counters> counts = std::make_shared<Counters>();
	std::shared_ptr<js_async_http::Dispatcher> transport = std::make_shared<js_async_http::Dispatcher>(MaxPending, 1);

public:
	bool started = false;
	std::vector<std::shared_ptr<Request>> waiting;
	void Start()
	{
		EnsureCurl();
		started = true;
		for (auto& request : waiting)
		{
			if (!transport->Commit(request->id))
				transport->Cancel(request->scriptgeneration, request->id);
		}
		waiting.clear();
	}
	std::shared_ptr<Request> Stage(const std::shared_ptr<Generation>& generation, const std::string& name,
		const std::string& body, const std::string& script = {},
		std::shared_ptr<js_async_http::Generation> scriptgeneration = {})
	{
		if (!generation || !generation->alive || body.size() > MaxBody || counts->pending >= MaxPending)
			return nullptr;
		const auto found = generation->endpoints.find(name);
		if (found == generation->endpoints.end())
			return nullptr;
		js_async_http::Policy policy;
		policy.ownerGeneration = generation->http;
		policy.headers = *found->second.headers;
		for (const auto& header : generation->ForUrl(script, found->second.url))
		{
			if (!policy.headers.emplace(header).second)
				return nullptr;
		}
		if (policy.headers.size() > MaxHeaders)
			return nullptr;
		policy.contentType = "application/json";
		policy.configuredEndpoint = true;
		policy.discard = true;
		policy.reportCancelled = true;
		policy.connectTimeout = 1000;
		policy.timeout = 2000;
		policy.maxResponse = MaxResponse;
		policy.maxTotal = MaxResponse;
		auto reservation = std::make_shared<Reservation>(counts);
		policy.completion = [stats = counts, reservation](bool sent)
		{
			reservation->completed = true;
			if (sent)
				++stats->sent;
			else
				++stats->failed;
		};
		if (!scriptgeneration)
			scriptgeneration = generation->http;
		const auto id = transport->Stage(scriptgeneration, "POST", found->second.url, body, std::move(policy));
		if (id.empty())
			return nullptr;
		auto request = std::make_shared<Request>(generation, transport, id);
		request->reservation = std::move(reservation);
		request->scriptgeneration = std::move(scriptgeneration);
		return request;
	}
	void Commit(std::shared_ptr<Request> request)
	{
		if (!request)
			return;
		request->reservation->committed = true;
		if (!started)
		{
			request->committed = true;
			waiting.push_back(std::move(request));
		}
		else
			request->Commit();
	}
	const Counters& Stats() const { return *counts; }
	void Stop()
	{
		transport->Stop();
		waiting.clear();
	}
};
} // namespace js_webhooks
