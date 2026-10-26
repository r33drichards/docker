/* SPDX-License-Identifier: MIT */
// Linux companion: owns one ssh child and exits when the module socket closes.
#include "../modules/reverse_tunnel.h"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

using Clock = std::chrono::steady_clock;
static bool Closed(int milliseconds)
{
	pollfd fd{0, POLLIN | POLLHUP, 0};
	int result;
	do { result = poll(&fd, 1, milliseconds); } while (result < 0 && errno == EINTR);
	// No inbound application messages. Any input/EOF/error means stop.
	return result != 0;
}
static void Stop(pid_t pid)
{
	kill(pid, SIGKILL);
	while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) { }
}
int main(int argc, char** argv)
{
	if (argc < 2 || std::string(argv[1]) != "/usr/bin/ssh") return 2;
	signal(SIGCHLD, SIG_DFL);
	signal(SIGPIPE, SIG_IGN);
	unsigned failures = 0;
	while (!Closed(0))
	{
		const auto started = Clock::now();
		const pid_t parent = getpid();
		pid_t pid = fork();
		if (!pid)
		{
			// Kernel kills SSH if this supervisor crashes. Check the fork race.
			if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(126);
			int nullfd = open("/dev/null", O_RDWR);
			if (nullfd < 0) _exit(126);
			for (int fd = 0; fd < 3; ++fd) if (dup2(nullfd, fd) < 0) _exit(126);
			if (nullfd > 2) close(nullfd);
			signal(SIGPIPE, SIG_DFL);
			execv("/usr/bin/ssh", argv + 1);
			_exit(127);
		}
		if (pid > 0)
		{
			if (write(1, "S", 1) != 1) { Stop(pid); return 0; }
			for (;;)
			{
				const pid_t result = waitpid(pid, nullptr, WNOHANG);
				if (result == pid || (result < 0 && errno != EINTR)) break;
				if (Closed(200)) { Stop(pid); return 0; }
			}
		}
		if (Clock::now() - started >= std::chrono::seconds(60)) failures = 0;
		if (write(1, "B", 1) != 1) return 0;
		const unsigned delay = reverse_tunnel::Backoff(failures);
		failures = std::min(failures + 1, 6U);
		if (Closed(static_cast<int>(delay * 1000))) return 0;
	}
}
