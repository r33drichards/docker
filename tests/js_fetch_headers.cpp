/* SPDX-License-Identifier: MIT */
#include "../modules/js_webhooks.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
void Check(bool condition, const std::string& message)
{
	if (!condition)
		throw std::runtime_error(message);
}

js_webhooks::Endpoint Rule(const std::string& url, const std::string& token)
{
	js_webhooks::Endpoint endpoint;
	endpoint.url = url;
	endpoint.headers = std::make_shared<const js_webhooks::Headers>(
		js_webhooks::Headers{{"xc-token", token}, {"authorization", "Bearer synthetic-test-only"}});
	return endpoint;
}

void TestExactRules()
{
	using namespace js_webhooks;
	const std::string script = "/config/admin.js";
	const std::string url = "https://Example.invalid/hooks/join?a=1&b=2";
	const auto key = ExactUrl(url);
	Check(key == "https://example.invalid:443/hooks/join?a=1&b=2", "canonical destination identity");
	for (const auto* equivalent : {"https://example.invalid/hooks/join?a=1&b=2",
			 "HTTPS://EXAMPLE.INVALID:443/hooks/join?a=1&b=2", "https://Example.invalid:0443/hooks/join?a=1&b=2"})
		Check(ExactUrl(equivalent) == key, "scheme/host case and effective port must match");
	Check(ExactUrl("https://example.invalid") == ExactUrl("https://example.invalid:443/"), "default root path");
	Check(ExactUrl("http://example.invalid/") == ExactUrl("http://EXAMPLE.INVALID:80/"), "HTTP effective port");
	Check(ExactUrl("https://example.invalid/hook?") != ExactUrl("https://example.invalid/hook"),
		"absent and empty query are distinct");

	HeaderRules rules;
	rules.emplace(std::make_pair(script, key), Rule(url, "synthetic-owner-token"));
	rules.emplace(std::make_pair("/config/other.js", key), Rule(url, "synthetic-other-token"));
	rules.emplace(std::make_pair(script, ExactUrl("https://example.invalid/another")),
		Rule("https://example.invalid/another", "synthetic-another-token"));
	Generation generation({}, std::move(rules));
	const auto expected = generation.ForUrl(script, url);
	Check(expected.at("xc-token") == "synthetic-owner-token", "matching script receives its configured header");
	Check(generation.ForUrl(script, "HTTPS://EXAMPLE.INVALID:443/hooks/join?a=1&b=2") == expected,
		"equivalent destination selects the same rule");
	Check(generation.ForUrl("/config/other.js", url).at("xc-token") == "synthetic-other-token",
		"rules are scoped to exact script identity");
	for (const auto* wrongscript : {"", "/config/ADMIN.js", "/config/admin.js.extra", "/other/admin.js"})
		Check(generation.ForUrl(wrongscript, url).empty(), "wrong script must receive no headers");
	for (const auto* different : {"http://example.invalid/hooks/join?a=1&b=2",
			 "https://example.invalid:444/hooks/join?a=1&b=2", "https://sub.example.invalid/hooks/join?a=1&b=2",
			 "https://example.invalid.attacker.invalid/hooks/join?a=1&b=2",
			 "https://notexample.invalid/hooks/join?a=1&b=2", "https://example.invalid./hooks/join?a=1&b=2",
			 "https://example.invalid/Hooks/join?a=1&b=2", "https://example.invalid/hooks/joined?a=1&b=2",
			 "https://example.invalid/hooks/join/child?a=1&b=2", "https://example.invalid/hooks/join/?a=1&b=2",
			 "https://example.invalid/hooks/join?b=2&a=1", "https://example.invalid/hooks/join?a=1&b=3",
			 "https://example.invalid/hooks/join?a=1&b=2&c=3", "https://example.invalid/hooks/join?a=1&b=%32",
			 "https://example.invalid/hooks/join", "https://example.invalid/hooks/join?"})
		Check(generation.ForUrl(script, different).empty(), "URL near miss must receive no headers");
	for (const auto* invalid : {"", "file:///tmp/hooks/join", "ftp://example.invalid/hooks/join",
			 "https://user@example.invalid/hooks/join?a=1&b=2", "https://:password@example.invalid/hooks/join?a=1&b=2",
			 "https://@example.invalid/hooks/join?a=1&b=2", "https://example.invalid/hooks/join?a=1&b=2#fragment",
			 "https://example.invalid/hooks/join?a=1&b=2#", "https://example.invalid/hooks/./join?a=1&b=2",
			 "https://example.invalid/hooks/x/../join?a=1&b=2", "https://example.invalid/hooks/.",
			 "https://example.invalid/hooks/..", "https://example.invalid/hooks\\join?a=1&b=2",
			 "https://example.invalid/hooks/join?bad value", "https://example.invalid/hooks/join\r\nX-Test: bad"})
	{
		Check(ExactUrl(invalid).empty(), "invalid destination identity accepted: " + std::string(invalid));
		Check(generation.ForUrl(script, invalid).empty(), "invalid destination must receive no headers");
	}
	const auto nul = url + std::string("\0ignored", 8);
	Check(ExactUrl(nul).empty() && generation.ForUrl(script, nul).empty(), "embedded NUL rejected before curl");
	Check(ExactUrl(std::string(2049, 'a')).empty(), "oversized identity rejected");
	Check(ExactUrl("https://example.invalid/a%2Fb") != ExactUrl("https://example.invalid/a/b"),
		"escaped path separators stay byte-exact");
	auto copy = generation.ForUrl(script, url);
	copy["xc-token"] = "attempted-override";
	copy.clear();
	Check(generation.ForUrl(script, url) == expected, "selected header copies cannot mutate host-owned rules");
	Check(!js_async_http::ValidUrl("https://127.0.0.1/hooks/join"),
		"header selection must not grant private destination access");
	Check(!js_async_http::ValidUrl("https://example.invalid:444/hooks/join"),
		"header selection must not grant nonstandard destination ports");
}

#ifdef JS_ASYNC_HTTP_TEST_LOOPBACK
js_async_http::Result Wait(js_async_http::Dispatcher& dispatcher,
	const std::shared_ptr<js_async_http::Generation>& generation, const std::string& id)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
	js_async_http::Result result;
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (dispatcher.Take(generation, id, result))
			return result;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	throw std::runtime_error("timed out waiting for fixture result");
}

void Await(const std::atomic<unsigned>& count, unsigned expected)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
	while (count.load() < expected && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	Check(count.load() == expected, "completion callback count");
}

void TestLegacyAdapter(const std::string& base)
{
	using namespace js_webhooks;
	const std::string script = "/config/admin.js", url = base + "/legacy";
	auto endpoint = ValidateEndpoint(url, true);
	endpoint.headers = std::make_shared<const Headers>(Headers{{"x-legacy", "synthetic-legacy"}});
	HeaderRules rules{{{script, ExactUrl(url)}, Rule(url, "synthetic-owner-token")}};
	auto generation = std::make_shared<Generation>(std::map<std::string, Endpoint>{{"legacy", endpoint}}, rules);
	generation->alive = true;
	Dispatcher dispatcher;
	dispatcher.Start();
	for (const auto& owner : {script, std::string("/config/untrusted.js")})
	{
		const auto body = owner == script ? "{}" : "{\"wrongscript\":true}";
		auto request = dispatcher.Stage(generation, "legacy", body, owner);
		Check(request != nullptr, "legacy adapter stages through shared transport");
		dispatcher.Commit(std::move(request));
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
	while (dispatcher.Stats().pending && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	Check(dispatcher.Stats().pending == 0 && dispatcher.Stats().sent == 2 && dispatcher.Stats().failed == 0,
		"legacy adapter drains and reports shared transport outcomes");

	// This must fail rather than establish a script-controlled override order.
	auto conflict = Rule(url, "synthetic-owner-token");
	conflict.headers = endpoint.headers;
	rules = {{{script, ExactUrl(url)}, conflict}};
	generation = std::make_shared<Generation>(std::map<std::string, Endpoint>{{"legacy", endpoint}}, rules);
	generation->alive = true;
	Check(!dispatcher.Stage(generation, "legacy", "{}", script), "conflicting legacy and URL headers rejected");
	dispatcher.Stop();
}

void TestTransport(const std::string& base)
{
	using namespace js_async_http;
	const std::string script = "/config/admin.js";
	js_webhooks::HeaderRules rules;
	for (const auto* path : {"/post", "/redirect", "/cancel", "/retired", "/discard", "/failure"})
	{
		const auto url = base + path;
		rules.emplace(std::make_pair(script, js_webhooks::ExactUrl(url)), Rule(url, "synthetic-owner-token"));
	}
	// Only this fixture executable allows loopback. Production configuration still
	// requires HTTPS and the ordinary public-destination fetch policy.
	js_webhooks::Generation config({}, std::move(rules));
	auto policyFor = [&](const std::string& path, const std::string& owner)
	{
		Policy policy;
		policy.headers = config.ForUrl(owner, base + path);
		return policy;
	};
	std::atomic<unsigned> cancelledCallbacks{0}, retiredCallbacks{0};
	Dispatcher dispatcher;
	auto generation = std::make_shared<Generation>(), other = std::make_shared<Generation>();
	const std::string body = "{\"headers\":{\"xc-token\":\"attempted-js-override\"},\"event\":\"synthetic\"}";
	auto policy = policyFor("/post", script);
	policy.contentType = "application/json";
	auto id = dispatcher.Stage(generation, "POST", base + "/post", body, policy);
	Check(!id.empty(), "stage header-injected POST");
	policy.headers["xc-token"] = "mutated-after-stage";
	Check(dispatcher.Commit(id) && !dispatcher.Commit(id), "commit once");
	auto result = Wait(dispatcher, generation, id);
	Check(result.ok && result.status == 200 && result.body == body, "injected POST preserves body");
	Check(Wait(dispatcher, generation, id).error == "expired or unknown HTTP request", "result consumed once");

	for (const auto& item : std::vector<std::pair<std::string, std::string>>{
			 {"/post", "/config/untrusted.js"}, {"/post/child", script}, {"/post?extra=1", script}})
	{
		id = dispatcher.Stage(generation, "POST", base + item.first, "{}", policyFor(item.first, item.second));
		Check(dispatcher.Commit(id), "commit nonmatching fixture request");
		Check(Wait(dispatcher, generation, id).ok, "nonmatching fixture completes without credentials");
	}
	id = dispatcher.Stage(generation, "POST", base + "/redirect", "{}", policyFor("/redirect", script));
	Check(dispatcher.Commit(id), "commit redirect request");
	result = Wait(dispatcher, generation, id);
	Check(result.ok && result.status == 302, "redirect response returned instead of followed");

	policy = policyFor("/cancel", script);
	policy.completion = [&](bool) { ++cancelledCallbacks; };
	id = dispatcher.Stage(generation, "POST", base + "/never-committed", "{}", policy);
	Check(!id.empty() && !dispatcher.Cancel(other, id), "another generation cannot cancel the request");
	Check(dispatcher.Cancel(generation, id) && !dispatcher.Commit(id), "cancel staged request before I/O");
	Check(!Wait(dispatcher, generation, id).ok, "cancelled staged request has no result");
	id = dispatcher.Stage(generation, "POST", base + "/cancel", "{}", policy);
	Check(dispatcher.Commit(id), "commit in-flight cancellation request");
	auto barrier = dispatcher.Stage(other, "GET", base + "/wait/cancel");
	Check(dispatcher.Commit(barrier), "commit cancellation barrier");
	Check(Wait(dispatcher, other, barrier).body == "ready", "cancel only after receiver observed POST");
	Check(dispatcher.Cancel(generation, id), "cancel in-flight request");
	Check(!Wait(dispatcher, generation, id).ok, "cancelled request cannot expose response");

	auto retired = std::make_shared<Generation>();
	policy = policyFor("/retired", script);
	policy.completion = [&](bool) { ++retiredCallbacks; };
	id = dispatcher.Stage(retired, "POST", base + "/retired", "{}", policy);
	Check(dispatcher.Commit(id), "commit old-generation request");
	barrier = dispatcher.Stage(other, "GET", base + "/wait/retired");
	Check(dispatcher.Commit(barrier), "commit retirement barrier");
	Check(Wait(dispatcher, other, barrier).body == "ready", "retire only after receiver observed POST");
	Check(!dispatcher.Cancel(other, id), "new generation cannot cancel old-generation job");
	dispatcher.Invalidate(retired);
	Check(!Wait(dispatcher, retired, id).ok, "retired generation cannot take result");
	Check(dispatcher.Stage(retired, "POST", base + "/never-retired", "{}").empty(), "retired generation cannot stage");
	Check(!dispatcher.Commit(id), "retired job cannot be recommitted");
	policy = policyFor("/post", script);
	policy.headers["xc-token"] = "synthetic-rotated-token";
	id = dispatcher.Stage(other, "POST", base + "/post", "{\"rotated\":true}", policy);
	Check(dispatcher.Commit(id) && Wait(dispatcher, other, id).ok, "new generation continues independently");
	dispatcher.Stop();
	Check(cancelledCallbacks == 0 && retiredCallbacks == 0, "cancelled and retired jobs suppress completion callbacks");
	Check(dispatcher.Stage(other, "GET", base + "/never-stopped").empty(), "stopped dispatcher rejects new requests");

	std::atomic<unsigned> completed{0}, successful{0};
	Dispatcher discard(1);
	for (const auto* path : {"/discard", "/failure"})
	{
		policy = policyFor(path, script);
		policy.discard = true;
		policy.completion = [&](bool ok)
		{
			if (ok)
				++successful;
			++completed;
		};
		id = discard.Stage(other, "POST", base + path, "{}", policy);
		Check(!id.empty() && discard.Commit(id), "discard job uses reclaimed capacity");
		Await(completed, std::string(path) == "/discard" ? 1 : 2);
		Check(Wait(discard, other, id).error == "expired or unknown HTTP request", "discard mode retains no result");
	}
	discard.Stop();
	Check(completed == 2 && successful == 1, "discard callback distinguishes HTTP success and failure");
}
#endif
} // namespace

int main(int argc, char** argv)
{
	try
	{
		TestExactRules();
#ifdef JS_ASYNC_HTTP_TEST_LOOPBACK
		Check(argc == 2, "usage: js_fetch_headers FIXTURE_URL");
		TestTransport(argv[1]);
		TestLegacyAdapter(argv[1]);
#else
		(void)argc;
		(void)argv;
		Check(!js_async_http::ValidUrl("http://127.0.0.1:12345/post"), "production cannot use loopback fixture bypass");
#endif
		std::cout << "PASS: exact URL/script rules, immutable host headers and shared transport lifecycle\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
