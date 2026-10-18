/* SPDX-License-Identifier: MIT */

#include <dlfcn.h>
#include <valgrind/memcheck.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
void Check(bool condition, const std::string& message)
{
	if (!condition)
		throw std::runtime_error(message);
}

void Progress(unsigned int generation, const char* phase)
{
	using Clock = std::chrono::steady_clock;
	static const auto started = Clock::now();
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
	std::fprintf(stderr, "HOST +%lldms generation=%u %s\n", static_cast<long long>(elapsed), generation, phase);
	std::fflush(stderr);
}

void Reload(const char* path, unsigned int expected_generation)
{
	Progress(expected_generation, "dlopen: start");
	void* library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!library)
		throw std::runtime_error(dlerror());
	Progress(expected_generation, "dlopen: complete; resolving entry point");
	dlerror();
	void* symbol = dlsym(library, "lifecycle_generation");
	const char* error = dlerror();
	if (error)
		throw std::runtime_error(error);
	using Run = unsigned int (*)();
	static_assert(sizeof(Run) == sizeof(symbol), "POSIX dlsym pointer size mismatch");
	Run run;
	std::memcpy(&run, &symbol, sizeof(run));
	Progress(expected_generation, "calling generation");
	Check(run() == expected_generation, "runtime failed or NODELETE state reset across dlclose");
	Progress(expected_generation, "generation complete; dlclose: start");
	Check(dlclose(library) == 0, "dlclose failed");
	void* retained = dlopen(path, RTLD_NOW | RTLD_NOLOAD | RTLD_LOCAL);
	Check(retained != nullptr, "NODELETE library was unmapped after dlclose");
	Check(dlclose(retained) == 0, "retained dlclose failed");
	Progress(expected_generation, "dlclose: complete; NODELETE verified");
}

struct Heap
{
	unsigned long lost = 0;
	unsigned long possible = 0;
	unsigned long reachable = 0;
	unsigned long suppressed = 0;
};

Heap Snapshot()
{
	Progress(0, "heap snapshot: start");
	VALGRIND_DO_QUICK_LEAK_CHECK;
	Heap heap;
	VALGRIND_COUNT_LEAKS(heap.lost, heap.possible, heap.reachable, heap.suppressed);
	Progress(0, "heap snapshot: complete");
	return heap;
}
} // namespace

int main(int argc, char** argv)
{
	try
	{
		std::setvbuf(stderr, nullptr, _IONBF, 0);
		Progress(0, "host main entered");
		Check(argc == 2, "usage: js_runtime_lifecycle_host SHARED_LIBRARY");
		Check(RUNNING_ON_VALGRIND != 0, "this test requires Valgrind Memcheck");
		// Warm both successful and terminated isolate paths and libc's thread cache.
		for (unsigned int generation = 1; generation <= 4; ++generation)
			Reload(argv[1], generation);
		// Initialize stdout's buffer before measuring reachable-byte growth.
		std::cout << "Measuring 24 NODELETE reload generations after warmup" << std::endl;
		const Heap before = Snapshot();
		for (unsigned int generation = 5; generation <= 28; ++generation)
		{
			Reload(argv[1], generation);
			std::cout << "Completed generation " << generation << std::endl;
		}
		const Heap after = Snapshot();
		std::cout << "Heap baseline/final bytes: lost=" << before.lost << '/' << after.lost
				  << ", possible=" << before.possible << '/' << after.possible << ", reachable=" << before.reachable
				  << '/' << after.reachable << ", suppressed=" << before.suppressed << '/' << after.suppressed << '\n';
		Check(before.lost == 0 && after.lost == 0, "definitely/indirectly lost heap bytes");
		Check(after.possible <= before.possible, "possibly-lost heap grew across reloads");
		Check(after.reachable <= before.reachable, "retained heap grew across reloads");
		Check(before.suppressed == 0 && after.suppressed == 0, "unexpected suppressed heap allocations");
		std::cout << "PASS: NODELETE state, successful/error/timeout destruction, persistent-handle ownership, stable "
					 "retained heap\n";
		return EXIT_SUCCESS;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return EXIT_FAILURE;
	}
}
