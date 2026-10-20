/* SPDX-License-Identifier: MIT */
#pragma once

// Owned, bounded webhook jobs. The worker never enters V8 or touches IRC state.
#include <curl/curl.h>
#include <atomic>
#include <condition_variable>
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

struct Endpoint
{
	std::string url;
	bool loopbackhttp = false;
};

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
