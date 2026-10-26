/* SPDX-License-Identifier: MIT */
#pragma once
#include "reverse_tunnel.h"
#include <cstdint>
#include <limits>
#include <memory>

namespace reverse_tunnel
{
// Transport operations must never block the daemon. Receive returns positive
// bytes, zero for EOF, -1 for would-block/interrupted, or -2 for an error.
struct Transport
{
	virtual ~Transport() = default;
	virtual int Start(const std::vector<std::string>& arguments) = 0;
	virtual int Receive(int socket, char* buffer, size_t size) = 0;
	virtual void Stop(int socket) = 0;
	virtual void Close(int socket) = 0;
};

class Controller
{
	struct Runtime
	{
		std::vector<std::string> args;
		uint64_t generation;
		int socket = -1;
		unsigned failures = 0;
		std::string state = "pending", failure = "none";
		uint64_t retry = 0, started = 0;
		bool stopping = false;
		Runtime(std::vector<std::string> a, uint64_t g) : args(std::move(a)), generation(g) { }
	};
	Transport& transport;
	std::map<std::string, std::unique_ptr<Runtime>> running;
	std::map<std::string, uint64_t> generations;
	uint64_t sequence = 0;

	void Failed(Runtime& r, uint64_t now)
	{
		if (now - r.started >= 60) r.failures = 0;
		r.retry = now + Backoff(r.failures);
		r.failures = std::min(r.failures + 1, 6U);
		r.state = "backoff";
	}
	void Observe(Runtime& r, uint64_t now)
	{
		// Bound reads; never let a misbehaving companion monopolize the loop.
		for (unsigned reads = 0; r.socket >= 0 && reads < 4; ++reads)
		{
			char buffer[64];
			const int count = transport.Receive(r.socket, buffer, sizeof(buffer));
			if (count == -1) break;
			if (count <= 0)
			{
				transport.Close(r.socket);
				r.socket = -1;
				if (!r.stopping)
				{
					if (r.failure == "none") r.failure = "companion-lost";
					Failed(r, now);
				}
				break;
			}
			// A retired generation may report late events. Only its EOF matters.
			if (r.stopping) continue;
			for (int i = 0; i < count; ++i)
			{
				if (buffer[i] == 'S') r.state = "ssh-running-unverified";
				else if (buffer[i] == 'X') { r.state = "ssh-exited"; r.failure = "ssh-exited"; }
				else if (buffer[i] == 'F') { r.state = "spawn-failed"; r.failure = "ssh-spawn-failed"; }
			}
		}
	}

public:
	Profiles profiles;
	Tunnels tunnels;
	explicit Controller(Transport& t) : transport(t) { }
	~Controller()
	{
		// Destruction/unload cancels all generations; no timer survives this owner.
		for (const auto& r : running) if (r.second->socket >= 0) transport.Close(r.second->socket);
	}
	void Submit(Profiles nextprofiles, Tunnels nexttunnels)
	{
		Validate(nextprofiles, nexttunnels);
		auto nextsequence = sequence;
		std::map<std::string, uint64_t> nextgenerations;
		for (const auto& next : nexttunnels)
		{
			const auto old = tunnels.find(next.first);
			if (old != tunnels.end() && old->second.profile == next.second.profile &&
				old->second.dynamic == next.second.dynamic && old->second.enabled == next.second.enabled &&
				Arguments(profiles.at(old->second.profile), old->second) == Arguments(nextprofiles.at(next.second.profile), next.second))
				nextgenerations[next.first] = generations.at(next.first);
			else
			{
				Require(nextsequence != std::numeric_limits<uint64_t>::max(), "generation limit reached; reload module");
				nextgenerations[next.first] = ++nextsequence;
			}
		}
		// Submission changes desired state only. No start/stop/read in this path.
		profiles.swap(nextprofiles);
		tunnels.swap(nexttunnels);
		generations.swap(nextgenerations);
		sequence = nextsequence;
	}
	void Reconcile(uint64_t now)
	{
		for (auto it = running.begin(); it != running.end(); )
		{
			auto& r = *it->second;
			const auto desired = tunnels.find(it->first);
			const bool current = desired != tunnels.end() && desired->second.enabled &&
				generations.at(it->first) == r.generation;
			if (!current && !r.stopping)
			{
				r.stopping = true;
				r.state = "stopping";
				if (r.socket >= 0) transport.Stop(r.socket);
			}
			Observe(r, now);
			if (r.stopping && r.socket < 0) it = running.erase(it);
			else ++it;
		}
		for (const auto& desired : tunnels)
		{
			// Retiring processes count towards the global process limit. A name's
			// replacement waits for EOF from its previous companion, not a PID.
			if (desired.second.enabled && !running.count(desired.first) && running.size() < 32)
				running.emplace(desired.first, std::make_unique<Runtime>(Arguments(profiles.at(desired.second.profile), desired.second), generations.at(desired.first)));
		}
		for (auto& entry : running)
		{
			auto& r = *entry.second;
			if (!r.stopping && r.socket < 0 && now >= r.retry)
			{
				r.socket = transport.Start(r.args);
				r.started = now;
				r.failure = "none";
				r.state = "starting";
				if (r.socket < 0) { r.failure = "companion-spawn-failed"; Failed(r, now); }
			}
		}
	}
	std::string Status(const std::string& name, uint64_t now) const
	{
		const auto& t = tunnels.at(name);
		const auto observed = running.find(name);
		const auto generation = generations.at(name);
		std::string result = name + " owner=" + (t.dynamic ? "dynamic" : "config") +
			" desired=" + (t.enabled ? "enabled" : "disabled") + " generation=" + std::to_string(generation);
		if (observed == running.end())
			return result + " observed-generation=0 state=" + (t.enabled ? "pending" : "stopped");
		const auto& r = *observed->second;
		return result + " observed-generation=" + std::to_string(r.generation) + " state=" + r.state +
			" reconcile=" + (generation == r.generation && t.enabled ? "current" : "pending") +
			" failures=" + std::to_string(r.failures) + " retry-in=" +
			std::to_string(!r.stopping && r.socket < 0 && r.retry > now ? r.retry - now : 0) + " last-failure=" + r.failure;
	}
};
} // namespace reverse_tunnel
