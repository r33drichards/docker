/* SPDX-License-Identifier: MIT */
// One-shot Linux companion: owns/reaps one SSH child. The daemon controller
// handles retries; this process only isolates child ownership and cleanup.
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static bool Closed(int milliseconds)
{
	pollfd fd{0, POLLIN | POLLHUP, 0};
	int result;
	do { result = poll(&fd, 1, milliseconds); } while (result < 0 && errno == EINTR);
	// A control socket half-close requests stop. There are no inbound messages.
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
	if (Closed(0)) return 0;
	const pid_t parent = getpid();
	pid_t pid = fork();
	if (!pid)
	{
		if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(126);
		int nullfd = open("/dev/null", O_RDWR);
		if (nullfd < 0) _exit(126);
		for (int fd = 0; fd < 3; ++fd) if (dup2(nullfd, fd) < 0) _exit(126);
		if (nullfd > 2) close(nullfd);
		signal(SIGPIPE, SIG_DFL);
		execv("/usr/bin/ssh", argv + 1);
		_exit(127);
	}
	if (pid < 0) { if (write(1, "F", 1) < 0) return 1; return 1; }
	if (write(1, "S", 1) != 1) { Stop(pid); return 0; }
	for (;;)
	{
		const pid_t result = waitpid(pid, nullptr, WNOHANG);
		if (result == pid || (result < 0 && errno != EINTR)) break;
		if (Closed(200)) { Stop(pid); return 0; }
	}
	if (write(1, "X", 1) < 0) return 1;
	return 0;
}
