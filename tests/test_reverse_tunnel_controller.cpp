/* SPDX-License-Identifier: MIT */
#include "../modules/reverse_tunnel_controller.h"
#include <cassert>
#include <cstring>
#include <iostream>
using namespace reverse_tunnel;

struct Fake final : Transport
{
	struct Channel { std::string data; bool eof = false, stopped = false, closed = false; };
	std::map<int, Channel> channels;
	int starts = 0, stops = 0, closes = 0;
	bool fail = false;
	int Start(const std::vector<std::string>&) override
	{
		++starts;
		if (fail) return -1;
		channels[starts] = {};
		return starts;
	}
	int Receive(int fd, char* buffer, size_t size) override
	{
		auto& c = channels.at(fd);
		assert(!c.closed);
		if (c.data.empty()) return c.eof ? 0 : -1;
		auto count = std::min(size, c.data.size());
		memcpy(buffer, c.data.data(), count);
		c.data.erase(0, count);
		return static_cast<int>(count);
	}
	void Stop(int fd) override { ++stops; channels.at(fd).stopped = true; }
	void Close(int fd) override { ++closes; assert(!channels.at(fd).closed); channels.at(fd).closed = true; }
	void Exit(int fd, const std::string& events = "X") { channels.at(fd).data = events; channels.at(fd).eof = true; }
};
static bool Has(const Controller& c, const std::string& text, uint64_t now = 0) { return c.Status("irc", now).find(text) != std::string::npos; }
int main()
{
	Profiles p{{"edge", {"gateway.example", "tunnel", "/key", "/hosts", "127.0.0.1", 22, 16667, 16670, 6667, 6697}}};
	Tunnels t{{"irc", {"edge", 16667, 6667, true, true}}};
	Fake f;
	{
		Controller c(f);
		c.Submit(p, t);
		assert(f.starts == 0 && f.stops == 0 && Has(c, "state=pending"));
		c.Reconcile(0);
		assert(f.starts == 1 && Has(c, "state=starting"));
		f.channels[1].data = "S";
		c.Reconcile(1);
		assert(Has(c, "state=ssh-running-unverified"));
		c.Submit(p, t); // Identical rehash neither replaces nor bumps generation.
		c.Reconcile(2);
		assert(f.starts == 1 && f.stops == 0 && Has(c, "generation=1 observed-generation=1"));
		try { auto bad = t; bad["irc"].listen = 1; c.Submit(p, bad); assert(false); } catch (const Error&) { }
		assert(c.tunnels.at("irc").listen == 16667 && f.stops == 0);

		// Child death and missing-companion EOF both reconcile with bounded backoff.
		f.Exit(1);
		c.Reconcile(3);
		assert(Has(c, "state=backoff") && Has(c, "retry-in=1", 3) && Has(c, "last-failure=ssh-exited"));
		c.Reconcile(3);
		assert(f.starts == 1);
		c.Reconcile(4);
		assert(f.starts == 2);
		f.Exit(2, "");
		c.Reconcile(5);
		assert(Has(c, "retry-in=2", 5) && Has(c, "last-failure=companion-lost"));
		c.Reconcile(6);
		assert(f.starts == 2);
		c.Reconcile(7);
		assert(f.starts == 3);

		// Submit disabled while active: no side effect until the timer reconciles.
		t["irc"].enabled = false;
		c.Submit(p, t);
		assert(f.stops == 0 && Has(c, "desired=disabled") && Has(c, "reconcile=pending"));
		c.Reconcile(8);
		assert(f.stops == 1 && Has(c, "state=stopping"));
		f.Exit(3, "SX"); // Late running/exit events from the retired generation.
		c.Reconcile(9);
		assert(Has(c, "state=stopped"));
		c.Reconcile(1000);
		assert(f.starts == 3); // No resurrection after retry deadline.

		t["irc"].enabled = true;
		c.Submit(p, t);
		c.Reconcile(1001);
		assert(f.starts == 4);
		// Delete/recreate the same name before a timer tick must not reuse ownership.
		c.Submit(p, {});
		c.Submit(p, t);
		assert(f.stops == 1);
		c.Reconcile(1002);
		assert(f.stops == 2 && f.starts == 4);
		c.Reconcile(1003); // Wait for old generation EOF; no overlapping launch.
		assert(f.starts == 4);
		f.Exit(4, "S");
		c.Reconcile(1004);
		assert(f.starts == 5 && Has(c, "generation=4 observed-generation=4") && Has(c, "state=starting"));
		// Profile changes also create a generation, stopping before replacement.
		p["edge"].host = "other.example";
		c.Submit(p, t);
		c.Reconcile(1005);
		assert(f.stops == 3 && f.starts == 5);
		f.Exit(5);
		c.Reconcile(1006);
		assert(f.starts == 6 && Has(c, "generation=5 observed-generation=5"));
		// Delete in backoff must cancel its scheduled recovery.
		f.Exit(6);
		c.Reconcile(1007);
		c.Submit(p, {});
		c.Reconcile(1008);
		c.Reconcile(2000);
		assert(f.starts == 6);
		c.Submit(p, t);
		c.Reconcile(2001);
		assert(f.starts == 7);
	}
	assert(f.channels.at(7).closed); // Unload closes ownership without waiting.

	// A long-lived generation resets the retry escalation after a later exit.
	Fake stable;
	Controller healthy(stable);
	healthy.Submit(p, t);
	healthy.Reconcile(0);
	stable.Exit(1);
	healthy.Reconcile(1);
	healthy.Reconcile(2);
	stable.Exit(2);
	healthy.Reconcile(3);
	healthy.Reconcile(5);
	stable.Exit(3);
	healthy.Reconcile(66);
	assert(Has(healthy, "failures=1") && Has(healthy, "retry-in=1", 66));

	// Spawn failures are bounded and cannot be accelerated by identical submits.
	Fake failed;
	failed.fail = true;
	Controller c(failed);
	c.Submit(p, t);
	uint64_t now = 0;
	for (unsigned i = 0; i < 10; ++i)
	{
		c.Reconcile(now);
		assert(failed.starts == static_cast<int>(i + 1));
		const auto delay = Backoff(i);
		c.Submit(p, t);
		c.Reconcile(now + delay - 1);
		assert(failed.starts == static_cast<int>(i + 1));
		now += delay;
	}
	t["irc"].enabled = false;
	c.Submit(p, t); // Disabling during spawn backoff cancels all future retries.
	c.Reconcile(now + 1000);
	assert(failed.starts == 10 && Has(c, "state=stopped"));

	Fake bounded;
	Controller limited(bounded);
	auto wide = p;
	wide["edge"].listenmax = 20000;
	Tunnels many;
	for (unsigned i = 0; i < 32; ++i) many["old" + std::to_string(i)] = {"edge", 17000 + i, 6667, true, true};
	limited.Submit(wide, many);
	limited.Reconcile(0);
	assert(bounded.starts == 32);
	many.clear();
	for (unsigned i = 0; i < 32; ++i) many["new" + std::to_string(i)] = {"edge", 17000 + i, 6667, true, true};
	limited.Submit(wide, many);
	limited.Reconcile(1);
	assert(bounded.starts == 32 && bounded.stops == 32);
	for (int fd = 1; fd <= 32; ++fd) bounded.Exit(fd, "S");
	limited.Reconcile(2);
	assert(bounded.starts == 64 && bounded.closes == 32);
	std::cout << "controller desired/observed state, generations, recovery, backoff, disable/delete and unload tests passed\n";
}
