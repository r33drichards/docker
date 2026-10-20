/* SPDX-License-Identifier: MIT */
#pragma once

// Owned asynchronous HTTP jobs. No worker enters V8 or accesses IRC objects.
#include <curl/curl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace js_async_http
{
constexpr std::size_t MaxJobs = 16, MaxBody = 512 * 1024, MaxRequestBody = 64 * 1024;
struct Generation
{
	std::atomic<bool> active{true};
};
struct Result
{
	bool ok = false;
	long status = 0;
	std::string body, error;
};
inline bool PublicAddress(const sockaddr* address)
{
	if (address->sa_family == AF_INET)
	{
		const auto ip = ntohl(reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr);
		// IANA IPv4 Special-Purpose Address Registry; keep exact prefix widths.
		// Conservatively exclude entire protocol-assignment/transition prefixes,
		// including their individual anycast exceptions. Multicast is not HTTP.
		// https://www.iana.org/assignments/iana-ipv4-special-registry/
		const auto in = [ip](uint32_t network, unsigned prefix)
		{ return (ip & (UINT32_MAX << (32 - prefix))) == network; };
		return !in(0x00000000, 8) && !in(0x0a000000, 8) && !in(0x64400000, 10) && !in(0x7f000000, 8) &&
			!in(0xa9fe0000, 16) && !in(0xac100000, 12) && !in(0xc0000000, 24) && !in(0xc0000200, 24) &&
			!in(0xc0586300, 24) && !in(0xc0a80000, 16) && !in(0xc6120000, 15) && !in(0xc6336400, 24) &&
			!in(0xcb007100, 24) && !in(0xe0000000, 4) && !in(0xf0000000, 4);
	}
	if (address->sa_family == AF_INET6)
	{
		const auto* b = reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr.s6_addr;
		return (b[0] & 0xe0) == 0x20 && !(b[0] == 0x20 && b[1] == 0x01 && b[2] <= 1) &&
			!(b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) && !(b[0] == 0x20 && b[1] == 0x02) &&
			!(b[0] == 0x3f && b[1] == 0xff && (b[2] & 0xf0) == 0);
	}
	return false;
}
inline bool FixtureUrl(const std::string& url)
{
#ifdef JS_ASYNC_HTTP_TEST_LOOPBACK
	return url.compare(0, 17, "http://127.0.0.1:") == 0;
#else
	(void)url;
	return false;
#endif
}
inline bool ValidUrl(const std::string& url)
{
	static const bool initialized = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
	if (!initialized || url.empty() || url.size() > 2048 || url.find_first_of("\r\n\t ") != std::string::npos ||
		url.find('\0') != std::string::npos)
		return false;
	std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
	if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), 0) != CURLUE_OK)
		return false;
	auto part = [&](CURLUPart which)
	{
		char* raw = nullptr;
		if (curl_url_get(parsed.get(), which, &raw, 0) != CURLUE_OK)
			return std::string();
		std::string value(raw);
		curl_free(raw);
		return value;
	};
	const auto scheme = part(CURLUPART_SCHEME), host = part(CURLUPART_HOST), port = part(CURLUPART_PORT);
	if ((scheme != "http" && scheme != "https") || host.empty() || !part(CURLUPART_USER).empty() ||
		!part(CURLUPART_PASSWORD).empty() || !part(CURLUPART_FRAGMENT).empty() ||
		(!FixtureUrl(url) && !port.empty() && port != (scheme == "https" ? "443" : "80")))
		return false;
	// Reject literal private addresses before enqueue; DNS destinations are also
	// validated at socket creation, avoiding resolve/connect TOCTOU rebinding.
	sockaddr_in v4{};
	v4.sin_family = AF_INET;
	if (inet_pton(AF_INET, host.c_str(), &v4.sin_addr) == 1)
		return PublicAddress(reinterpret_cast<sockaddr*>(&v4)) || FixtureUrl(url);
	const auto ipv6 =
		host.size() > 2 && host.front() == '[' && host.back() == ']' ? host.substr(1, host.size() - 2) : host;
	sockaddr_in6 v6{};
	v6.sin6_family = AF_INET6;
	if (inet_pton(AF_INET6, ipv6.c_str(), &v6.sin6_addr) == 1)
		return PublicAddress(reinterpret_cast<sockaddr*>(&v6));
	return true;
}
struct Policy
{
	std::map<std::string, std::string> headers;
	std::string contentType;
	bool configuredEndpoint = false, discard = false, reportCancelled = false;
	long connectTimeout = 3000, timeout = 8000;
	std::size_t maxResponse = MaxBody, maxTotal = 0;
	// Optional enclosing configuration generation; both lifetimes must remain live.
	std::shared_ptr<Generation> ownerGeneration;
	std::function<void(bool)> completion;
};
inline bool ConfiguredUrl(const std::string& url)
{
	std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> p(curl_url(), curl_url_cleanup);
	if (url.empty() || url.size() > 2048 || url.find('\0') != std::string::npos)
		return false;
	if (!p || curl_url_set(p.get(), CURLUPART_URL, url.c_str(), 0) != CURLUE_OK)
		return false;
	auto part = [&](CURLUPart which)
	{
		char* raw = nullptr;
		if (curl_url_get(p.get(), which, &raw, 0) != CURLUE_OK)
			return std::string();
		std::string out(raw);
		curl_free(raw);
		return out;
	};
	return (part(CURLUPART_SCHEME) == "https" ||
			   (part(CURLUPART_SCHEME) == "http" &&
				   (part(CURLUPART_HOST) == "127.0.0.1" || part(CURLUPART_HOST) == "[::1]"))) &&
		part(CURLUPART_USER).empty() && part(CURLUPART_PASSWORD).empty() && part(CURLUPART_FRAGMENT).empty() &&
		url.find_first_of("\r\n\t ") == std::string::npos;
}
class Dispatcher
{
	using Clock = std::chrono::steady_clock;
	struct Job
	{
		std::string id, method, url, body;
		std::shared_ptr<Generation> generation;
		std::atomic<bool> cancelled{false};
		bool committed = false, ready = false;
		Policy policy;
		bool Active() const
		{
			return generation->active && (!policy.ownerGeneration || policy.ownerGeneration->active);
		}
		Result result;
		Clock::time_point created = Clock::now(), completed;
	};
	std::mutex mutex;
	std::condition_variable wake;
	std::map<std::string, std::shared_ptr<Job>> entries;
	std::deque<std::shared_ptr<Job>> queue;
	std::vector<std::thread> workers;
	std::atomic<bool> stopping{false};
	std::string prefix;
	unsigned long long sequence = 0;
	const std::size_t maxJobs, workerCount;
	struct Transfer
	{
		Dispatcher& owner;
		Job& job;
		std::string body;
		std::size_t headers = 0;
		bool Cancelled() const { return owner.stopping || job.cancelled || !job.Active(); }
	};
	static std::size_t Write(char* data, std::size_t size, std::size_t count, void* user)
	{
		auto& t = *static_cast<Transfer*>(user);
		if ((size && count > MaxBody / size) || t.Cancelled())
			return 0;
		const auto total = size * count;
		if (total > t.job.policy.maxResponse - t.body.size() ||
			(t.job.policy.maxTotal && t.headers + t.body.size() + total > t.job.policy.maxTotal))
			return 0;
		try
		{
			t.body.append(data, total);
		}
		catch (...)
		{
			return 0;
		}
		return total;
	}
	static std::size_t Header(char*, std::size_t size, std::size_t count, void* user)
	{
		auto& t = *static_cast<Transfer*>(user);
		if ((size && count > 65536 / size) || t.Cancelled())
			return 0;
		const auto total = size * count;
		if (total > 65536 - t.headers ||
			(t.job.policy.maxTotal && t.headers + t.body.size() + total > t.job.policy.maxTotal))
			return 0;
		t.headers += total;
		return total;
	}
	static int Progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
	{
		return static_cast<Transfer*>(user)->Cancelled() ? 1 : 0;
	}
	static curl_socket_t Open(void* user, curlsocktype purpose, curl_sockaddr* address)
	{
		const auto& t = *static_cast<Transfer*>(user);
		const bool local =
			(address->family == AF_INET &&
				ntohl(reinterpret_cast<const sockaddr_in*>(&address->addr)->sin_addr.s_addr) == 0x7f000001) ||
			(address->family == AF_INET6 &&
				IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<const sockaddr_in6*>(&address->addr)->sin6_addr));
		const bool fixture =
			(FixtureUrl(t.job.url) && local) || (t.job.policy.configuredEndpoint && ConfiguredUrl(t.job.url));
		if (t.Cancelled() || purpose != CURLSOCKTYPE_IPCXN || (!PublicAddress(&address->addr) && !fixture))
			return CURL_SOCKET_BAD;
		return ::socket(address->family, address->socktype, address->protocol);
	}
	Result Fetch(Job& job)
	{
		Result result;
		result.error = "HTTP transport failed";
		std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
		if (!curl)
			return result;
		Transfer t{*this, job, {}, 0};
		curl_slist* rawheaders = nullptr;
		auto addheader = [&](const std::string& line)
		{
			auto* next = curl_slist_append(rawheaders, line.c_str());
			if (!next)
				return false;
			rawheaders = next;
			return true;
		};
		bool headersok = addheader("Expect:");
		if (!job.policy.contentType.empty())
			headersok = headersok && addheader("Content-Type: " + job.policy.contentType);
		for (const auto& header : job.policy.headers)
			headersok = headersok && addheader(header.first + ": " + header.second);
		std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(rawheaders, curl_slist_free_all);
		if (!headersok)
			return result;
		auto set = [&](CURLoption option, auto value)
		{ return curl_easy_setopt(curl.get(), option, value) == CURLE_OK; };
		if (!set(CURLOPT_URL, job.url.c_str()) || !set(CURLOPT_PROTOCOLS_STR, "http,https") ||
			!set(CURLOPT_FOLLOWLOCATION, 0L) || !set(CURLOPT_PROXY, "") || !set(CURLOPT_NETRC, CURL_NETRC_IGNORED) ||
			!set(CURLOPT_SSL_VERIFYPEER, 1L) || !set(CURLOPT_SSL_VERIFYHOST, 2L) || !set(CURLOPT_NOSIGNAL, 1L) ||
			!set(CURLOPT_CONNECTTIMEOUT_MS, job.policy.connectTimeout) ||
			!set(CURLOPT_TIMEOUT_MS, job.policy.timeout) || !set(CURLOPT_HTTPHEADER, headers.get()) ||
			!set(CURLOPT_USERAGENT, "InspIRCd-JavaScript/1.0") || !set(CURLOPT_WRITEFUNCTION, &Write) ||
			!set(CURLOPT_WRITEDATA, &t) || !set(CURLOPT_HEADERFUNCTION, &Header) || !set(CURLOPT_HEADERDATA, &t) ||
			!set(CURLOPT_OPENSOCKETFUNCTION, &Open) || !set(CURLOPT_OPENSOCKETDATA, &t) ||
			!set(CURLOPT_XFERINFOFUNCTION, &Progress) || !set(CURLOPT_XFERINFODATA, &t) ||
			!set(CURLOPT_NOPROGRESS, 0L) ||
			!set(CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(job.policy.maxResponse)))
			return result;
		if (job.method == "POST" &&
			(!set(CURLOPT_POST, 1L) || !set(CURLOPT_POSTFIELDS, job.body.data()) ||
				!set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(job.body.size()))))
			return result;
		if (t.Cancelled() || curl_easy_perform(curl.get()) != CURLE_OK)
			return result;
		if (curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &result.status) != CURLE_OK)
			return result;
		result.ok = true; // HTTP 4xx/5xx are completed HTTP responses, not transport errors.
		result.body = std::move(t.body);
		result.error.clear();
		return result;
	}
	void Expire()
	{
		const auto now = Clock::now();
		for (auto it = entries.begin(); it != entries.end();)
		{
			auto& job = *it->second;
			if (!job.Active() || job.cancelled || (job.ready && now - job.completed > std::chrono::seconds(60)) ||
				(!job.committed && now - job.created > std::chrono::seconds(60)))
			{
				job.cancelled = true;
				it = entries.erase(it);
			}
			else
				++it;
		}
		queue.erase(std::remove_if(queue.begin(), queue.end(), [](const auto& job) { return job->cancelled.load(); }),
			queue.end());
	}
	void Run() noexcept
	{
		for (;;)
		{
			std::shared_ptr<Job> job;
			{
				std::unique_lock<std::mutex> lock(mutex);
				wake.wait(lock, [&] { return stopping || !queue.empty(); });
				if (stopping)
					return;
				job = std::move(queue.front());
				queue.pop_front();
			}
			const bool attempted = !job->cancelled && job->Active();
			Result result;
			try
			{
				result = Fetch(*job);
			}
			catch (...)
			{
				result.error = "HTTP transport failed";
			}
			std::lock_guard<std::mutex> lock(mutex);
			if (!job->cancelled && job->Active())
			{
				job->result = std::move(result);
				job->ready = true;
				job->completed = Clock::now();
				if (job->policy.completion)
					job->policy.completion(job->result.ok && job->result.status >= 200 && job->result.status < 300);
				if (job->policy.discard)
					entries.erase(job->id);
			}
			else
			{
				if (attempted && job->policy.reportCancelled && job->policy.completion)
					job->policy.completion(false);
				entries.erase(job->id);
			}
		}
	}

	static std::string NewPrefix()
	{
		static std::atomic<unsigned long long> instances{0};
		return std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "-" +
			std::to_string(++instances) + "-";
	}

public:
	explicit Dispatcher(std::size_t limit = MaxJobs, std::size_t count = 2)
		: prefix(NewPrefix())
		, maxJobs(limit)
		, workerCount(count)
	{
	}
	~Dispatcher() { Stop(); }
	Dispatcher(const Dispatcher&) = delete;
	Dispatcher& operator=(const Dispatcher&) = delete;
	std::string Stage(const std::shared_ptr<Generation>& generation, const std::string& method, const std::string& url,
		const std::string& body = {}, Policy policy = {})
	{
		if (!generation || !generation->active || (policy.ownerGeneration && !policy.ownerGeneration->active) ||
			(method != "GET" && method != "POST") ||
			(!ValidUrl(url) && !(policy.configuredEndpoint && ConfiguredUrl(url))) || body.size() > MaxRequestBody ||
			(method == "GET" && !body.empty()))
			return {};
		std::lock_guard<std::mutex> lock(mutex);
		Expire();
		if (stopping || entries.size() >= maxJobs)
			return {};
		auto job = std::make_shared<Job>();
		job->id = prefix + std::to_string(++sequence);
		job->generation = generation;
		job->method = method;
		job->url = url;
		job->body = body;
		job->policy = std::move(policy);
		entries.emplace(job->id, job);
		return job->id;
	}
	bool Commit(const std::string& id)
	{
		std::lock_guard<std::mutex> lock(mutex);
		Expire();
		auto found = entries.find(id);
		if (stopping || found == entries.end() || found->second->committed)
			return false;
		if (workers.empty())
		{
			const auto* version = curl_version_info(CURLVERSION_NOW);
			if (!version || version->version_num < 0x075500 || !(version->features & CURL_VERSION_ASYNCHDNS) ||
				!(version->features & CURL_VERSION_SSL) || !(version->features & CURL_VERSION_THREADSAFE))
				return false;
			for (std::size_t i = 0; i < workerCount; ++i)
				workers.emplace_back([this] { Run(); });
		}
		queue.push_back(found->second);
		found->second->committed = true;
		wake.notify_one();
		return true;
	}
	bool Take(const std::shared_ptr<Generation>& generation, const std::string& id, Result& result)
	{
		std::lock_guard<std::mutex> lock(mutex);
		Expire();
		auto found = entries.find(id);
		if (!generation || !generation->active || found == entries.end() || found->second->generation != generation)
		{
			result = {};
			result.error = "expired or unknown HTTP request";
			return true;
		}
		if (!found->second->ready)
			return false;
		result = std::move(found->second->result);
		entries.erase(found);
		return true;
	}
	bool Cancel(const std::shared_ptr<Generation>& generation, const std::string& id)
	{
		std::lock_guard<std::mutex> lock(mutex);
		auto found = entries.find(id);
		if (found == entries.end() || found->second->generation != generation)
			return false;
		found->second->cancelled = true;
		entries.erase(found);
		Expire();
		return true;
	}
	void Invalidate(const std::shared_ptr<Generation>& generation)
	{
		if (!generation)
			return;
		generation->active = false;
		std::lock_guard<std::mutex> lock(mutex);
		Expire();
	}
	void Stop()
	{
		stopping = true;
		wake.notify_all();
		for (auto& worker : workers)
			if (worker.joinable())
				worker.join();
		workers.clear();
		std::lock_guard<std::mutex> lock(mutex);
		queue.clear();
		entries.clear();
	}
};
} // namespace js_async_http
