#include "js_async_http.h"
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

static void Check(bool value)
{
	if (!value)
		throw std::runtime_error("HTTP assertion failed");
}
static bool Address(const char* value)
{
	sockaddr_in a{};
	a.sin_family = AF_INET;
	Check(inet_pton(AF_INET, value, &a.sin_addr) == 1);
	return js_async_http::PublicAddress(reinterpret_cast<sockaddr*>(&a));
}
#ifdef JS_ASYNC_HTTP_TEST_LOOPBACK
class Fixture
{
	int fd;
	std::thread thread;
	std::atomic<bool> stop{false};

public:
	std::string url;
	std::atomic<unsigned> requests{0};
	Fixture()
	{
		fd = socket(AF_INET, SOCK_STREAM, 0);
		Check(fd >= 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		Check(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
		socklen_t length = sizeof(addr);
		Check(getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &length) == 0);
		Check(listen(fd, 4) == 0);
		url = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port));
		thread = std::thread(
			[this]
			{
				while (!stop)
				{
					fd_set fds;
					FD_ZERO(&fds);
					FD_SET(fd, &fds);
					timeval timeout{0, 100000};
					if (select(fd + 1, &fds, nullptr, nullptr, &timeout) <= 0)
						continue;
					int client = accept(fd, nullptr, nullptr);
					if (client < 0)
						continue;
					timeval receiveTimeout{2, 0};
					setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &receiveTimeout, sizeof(receiveTimeout));
					std::string request;
					char buffer[4096];
					while (request.find("\r\n\r\n") == std::string::npos)
					{
						auto n = recv(client, buffer, sizeof(buffer), 0);
						if (n <= 0)
							break;
						request.append(buffer, n);
					}
					++requests;
					if (request.find(" /slow ") != std::string::npos)
						std::this_thread::sleep_for(std::chrono::milliseconds(200));
					std::string body = "fixture-response";
					if (request.find("POST ") == 0)
					{
						auto start = request.find("\r\n\r\n") + 4;
						while (request.size() < start + 3)
						{
							auto n = recv(client, buffer, sizeof(buffer), 0);
							if (n <= 0)
								break;
							request.append(buffer, n);
						}
						body = request.substr(start);
					}
					if (request.find(" /oversize ") != std::string::npos)
						body.assign(js_async_http::MaxBody + 1, 'a');
					const bool redirect = request.find(" /redirect ") != std::string::npos;
					const std::string response = std::string("HTTP/1.1 ") +
						(redirect ? "302 Found\r\nLocation: http://169.254.169.254/\r\n" : "200 OK\r\n") +
						"Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
					std::size_t sent = 0;
					while (sent < response.size())
					{
						auto n = send(client, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
						if (n <= 0)
							break;
						sent += n;
					}
					close(client);
				}
			});
	}
	~Fixture()
	{
		stop = true;
		thread.join();
		close(fd);
	}
};
static js_async_http::Result Wait(
	js_async_http::Dispatcher& d, const std::shared_ptr<js_async_http::Generation>& g, const std::string& id)
{
	js_async_http::Result r;
	for (unsigned i = 0; i < 1000; ++i)
	{
		if (d.Take(g, id, r))
			return r;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	throw std::runtime_error("HTTP fixture timed out");
}
#endif
int main()
{
	using namespace js_async_http;
	Check(ValidUrl("https://example.com/feed?x=1"));
	for (auto url : {"file:///etc/passwd", "ftp://example.com/", "https://user:pass@example.com/",
			 "https://example.com/#x", "https://example.com:444/", "http://127.0.0.1/", "http://[::1]/",
			 "http://169.254.169.254/", "http://10.1.1.1/"})
		Check(!ValidUrl(url));
	for (auto ip : {"0.1.1.1", "10.1.1.1", "127.1.1.1", "169.254.169.254", "100.64.0.1", "172.16.0.1", "192.168.0.1",
			 "198.18.0.1", "224.0.0.1"})
		Check(!Address(ip));
	Check(Address("8.8.8.8"));
	// Exact special-prefix boundaries, including previously overblocked /16s.
	for (auto ip : {"192.0.0.0", "192.0.0.255", "192.0.2.0", "192.0.2.255", "192.88.99.0", "192.88.99.255",
			 "198.51.100.0", "198.51.100.255", "203.0.113.0", "203.0.113.255", "198.18.0.0", "198.19.255.255",
			 "100.64.0.0", "100.127.255.255", "172.16.0.0", "172.31.255.255", "192.168.0.0", "192.168.255.255",
			 "240.0.0.0", "255.255.255.255"})
		Check(!Address(ip));
	for (auto ip :
		{"192.0.1.0", "192.0.1.255", "192.0.3.0", "192.2.0.1", "192.88.98.255", "192.88.100.0", "198.51.99.255",
			"198.51.101.0", "203.0.112.255", "203.0.114.0", "198.17.255.255", "198.20.0.0", "100.63.255.255",
			"100.128.0.0", "172.15.255.255", "172.32.0.0", "192.167.255.255", "192.169.0.0", "223.255.255.255"})
		Check(Address(ip));
	Dispatcher d;
	auto g = std::make_shared<Generation>(), other = std::make_shared<Generation>();
	Check(d.Stage(g, "PUT", "https://example.com/").empty());
	Check(d.Stage(g, "GET", "https://example.com/", "body").empty());
	Check(d.Stage(g, "POST", "https://example.com/", std::string(MaxRequestBody + 1, 'x')).empty());
	std::vector<std::string> ids;
	for (std::size_t i = 0; i < MaxJobs; ++i)
	{
		auto id = d.Stage(g, "GET", "https://example.com/");
		Check(!id.empty());
		ids.push_back(id);
	}
	Check(d.Stage(g, "GET", "https://example.com/").empty());
	Result result;
	Check(!d.Take(g, ids[0], result));
	Check(d.Take(other, ids[0], result));
	Check(!result.ok);
	Check(!d.Cancel(other, ids[0]));
	Check(d.Cancel(g, ids[0]));
	Check(!d.Commit(ids[0]));
	d.Invalidate(g);
	Check(!d.Commit(ids[1]));
	Check(d.Stage(g, "GET", "https://example.com/").empty());
	g = std::make_shared<Generation>();
#ifdef JS_ASYNC_HTTP_TEST_LOOPBACK
	Fixture fixture;
	auto id = d.Stage(g, "GET", fixture.url + "/");
	Check(!id.empty());
	std::this_thread::sleep_for(std::chrono::milliseconds(40));
	Check(fixture.requests == 0); // staging never starts I/O
	Check(d.Commit(id));
	Check(!d.Commit(id));
	auto r = Wait(d, g, id);
	Check(r.ok && r.status == 200 && r.body == "fixture-response");
	id = d.Stage(g, "POST", fixture.url + "/", std::string("a\0b", 3));
	Check(d.Commit(id));
	r = Wait(d, g, id);
	Check(r.ok && r.body == std::string("a\0b", 3));
	id = d.Stage(g, "GET", fixture.url + "/oversize");
	Check(d.Commit(id));
	r = Wait(d, g, id);
	Check(!r.ok && r.body.empty());
	const auto before = fixture.requests.load();
	id = d.Stage(g, "GET", fixture.url + "/redirect");
	Check(d.Commit(id));
	r = Wait(d, g, id);
	Check(r.ok && r.status == 302 && fixture.requests == before + 1);
	id = d.Stage(g, "GET", fixture.url + "/slow");
	const auto prior = fixture.requests.load();
	Check(d.Commit(id));
	for (unsigned i = 0; i < 500 && fixture.requests == prior; ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	Check(fixture.requests == prior + 1);
	Check(d.Cancel(g, id));
	r = Wait(d, g, id);
	Check(!r.ok && r.body.empty());
	id = d.Stage(g, "GET", fixture.url + "/slow");
	Check(d.Commit(id));
	d.Invalidate(g);
	r = Wait(d, g, id);
	Check(!r.ok && r.body.empty());
	g = std::make_shared<Generation>();
	id = d.Stage(g, "GET", fixture.url + "/");
	Check(d.Cancel(g, id));
	Check(!d.Commit(id));
#endif
	d.Stop();
	Check(d.Stage(g, "GET", "https://example.com/").empty());
	std::cout << "async HTTP tests passed\n";
}
