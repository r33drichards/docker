/* SPDX-License-Identifier: MIT */
#pragma once
#include "reverse_tunnel_controller.h"
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace reverse_tunnel
{
// Never retain a PID: InspIRCd ignores SIGCHLD and the kernel auto-reaps helpers.
// Socket lifetime provides ownership even if a PID has already been recycled.
inline int Start(const std::vector<std::string>& arguments)
{
	int pair[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair)) return -1;
	std::vector<char*> argv;
	argv.push_back(const_cast<char*>("/usr/local/libexec/inspircd-tunnel"));
	for (const auto& arg : arguments) argv.push_back(const_cast<char*>(arg.c_str()));
	argv.push_back(nullptr);
	char path[] = "PATH=/usr/bin:/bin";
	char locale[] = "LC_ALL=C";
	char* env[] = {path, locale, nullptr};
	const long limit = sysconf(_SC_OPEN_MAX);
	if (limit < 0) { close(pair[0]); close(pair[1]); return -1; }
	const pid_t pid = fork();
	if (!pid)
	{
		// Only async-signal-safe calls between fork and exec (rehash has threads).
		if (dup2(pair[1], 0) < 0 || dup2(pair[1], 1) < 0) _exit(126);
		int nullfd = open("/dev/null", O_WRONLY);
		if (nullfd < 0 || dup2(nullfd, 2) < 0) _exit(126);
		for (long fd = 3; fd < limit; ++fd) close(static_cast<int>(fd));
		execve(argv[0], argv.data(), env);
		_exit(127);
	}
	close(pair[1]);
	if (pid < 0) { close(pair[0]); return -1; }
	if (fcntl(pair[0], F_SETFL, O_NONBLOCK) < 0) { close(pair[0]); return -1; }
	return pair[0];
}
class ProcessTransport final : public Transport
{
public:
	int Start(const std::vector<std::string>& args) override { return reverse_tunnel::Start(args); }
	int Receive(int socket, char* buffer, size_t size) override
	{
		const auto count = read(socket, buffer, size);
		if (count >= 0) return static_cast<int>(count);
		return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? -1 : -2;
	}
	void Stop(int socket) override { shutdown(socket, SHUT_WR); }
	void Close(int socket) override { close(socket); }
};
} // namespace reverse_tunnel
