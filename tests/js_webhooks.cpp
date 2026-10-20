/* SPDX-License-Identifier: MIT */
#include "../modules/js_webhooks.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <vector>
using namespace js_webhooks;
#include "js_webhook_headers.inc"

static void Drain(Dispatcher& dispatcher)
{
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(8);
	while (dispatcher.Stats().pending && std::chrono::steady_clock::now() < end)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	assert(dispatcher.Stats().pending == 0);
}

static void Rejected(const std::shared_ptr<Request>& request)
{
	assert(!request);
}

int main(int argc, char** argv)
{
	assert(argc == 4);
	HeaderTests(argv[3]);
	const std::string base = argv[1];
	assert(ValidName("joins_1-A") && !ValidName("") && !ValidName("bad name") && !ValidName(std::string(65, 'x')));
	for (const auto& url : {"http://example.invalid/", "file:///tmp/test", "https://user:pass@example.invalid/",
			 "https://example.invalid/#fragment", "https://example.invalid/\r\nX:bad", "https://example.invalid/ bad"})
	{
		bool rejected = false;
		try
		{
			ValidateEndpoint(url);
		}
		catch (const std::runtime_error&)
		{
			rejected = true;
		}
		assert(rejected);
	}
	assert(ValidateEndpoint("https://example.invalid/path?token=synthetic").url.size() > 0);
	bool rejected = false;
	try
	{
		ValidateEndpoint("http://localhost/", true);
	}
	catch (const std::runtime_error&)
	{
		rejected = true;
	}
	assert(rejected); // DNS names cannot turn the loopback exception into arbitrary HTTP.
	auto generation =
		std::make_shared<Generation>(std::map<std::string, Endpoint>{{"joins", ValidateEndpoint(base + "/ok", true)}});
	Dispatcher bounded;
	Rejected(bounded.Stage(generation, "joins", "{}")); // Inactive candidate cannot post.
	generation->alive = true;
	Rejected(bounded.Stage(generation, "missing", "{}"));
	Rejected(bounded.Stage(generation, "joins", std::string(MaxBody + 1, 'x')));
	std::vector<std::shared_ptr<Request>> reservations;
	for (std::size_t i = 0; i < MaxPending; ++i)
	{
		auto request = bounded.Stage(generation, "joins", "{}");
		assert(request);
		reservations.push_back(std::move(request));
	}
	Rejected(bounded.Stage(generation, "joins", "{}"));
	assert(bounded.Stats().pending == MaxPending);
	reservations.clear(); // Failure/denial/candidate rollback frees staged requests without I/O.
	assert(bounded.Stats().pending == 0);
	for (std::size_t i = 0; i < MaxPending; ++i)
		bounded.Commit(bounded.Stage(generation, "joins", "{}"));
	Rejected(bounded.Stage(generation, "joins", "{}"));
	generation->alive = false;
	bounded.Start();
	Drain(bounded);
	assert(bounded.Stats().discarded == MaxPending && bounded.Stats().sent == 0);
	bounded.Stop();

	std::map<std::string, Endpoint> endpoints;
	for (const std::string name : {"ok", "redirect", "failure", "large", "slow"})
		endpoints.emplace(name, ValidateEndpoint(base + "/" + name, true));
	endpoints.emplace("tls", ValidateEndpoint(argv[2]));
	AddHeader(endpoints.at("ok"), "xc-token", std::string(argv[3]) + "/secret");
	AddHeader(endpoints.at("redirect"), "Authorization", std::string(argv[3]) + "/secret");
	AddHeader(endpoints.at("tls"), "Authorization", std::string(argv[3]) + "/secret");
	generation = std::make_shared<Generation>(std::move(endpoints));
	generation->alive = true;
	Dispatcher transport;
	transport.Start();
	const std::string body = "{\"event\":\"transport.test\",\"nick\":\"\xce\xbb\"}";
	for (const std::string name : {"ok", "redirect", "failure", "large", "slow", "tls"})
	{
		auto request = transport.Stage(generation, name, body);
		assert(request);
		transport.Commit(std::move(request));
	}
	Drain(transport);
	assert(transport.Stats().sent == 1 && transport.Stats().failed == 5);
	transport.Stop();
	transport.Stop(); // Idempotent shutdown.
	Rejected(transport.Stage(generation, "ok", "{}"));

	Dispatcher cancellation;
	cancellation.Start();
	auto request = cancellation.Stage(generation, "slow", "{}");
	assert(request);
	cancellation.Commit(std::move(request));
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	const auto start = std::chrono::steady_clock::now();
	cancellation.Stop();
	assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));
	assert(cancellation.Stats().pending == 0);
	std::cout << "PASS: URL policy, disabled candidate, staged/queued bounds, rollback, retired generation, HTTP "
				 "status/response/time limits, TLS rejection, shutdown\n";
}
