/*
 * The image remains non-root by default. A deployment may explicitly start this
 * bootstrap as root to initialize its root-owned persistent volume. No shell,
 * configuration, certificate or token helper runs until privileges are dropped.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const uid_t daemon_uid = 10000;
static const gid_t daemon_gid = 10000;
static const int directory_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

static int fail(const char* message)
{
	fprintf(stderr, "Persistent state bootstrap: %s; refusing startup.\n", message);
	return EXIT_FAILURE;
}

static int safe_parent(int fd)
{
	struct stat info;
	if (fstat(fd, &info) != 0 || !S_ISDIR(info.st_mode) || (info.st_uid != 0 && info.st_uid != daemon_uid) ||
		(info.st_mode & 0022) != 0)
		return fail("unsafe volume parent");
	return 0;
}

static int private_directory(int parent, const char* name)
{
	if (geteuid() == 0 && mkdirat(parent, name, 0700) != 0 && errno != EEXIST)
		return -1;
	int directory = openat(parent, name, directory_flags);
	struct stat info;
	if (directory < 0)
		return -1;
	if (fstat(directory, &info) != 0 || (info.st_mode & 07777) != 0700 ||
		(info.st_uid != daemon_uid && !(geteuid() == 0 && info.st_uid == 0)))
		goto error;
	/* Only the fixed directory inode can change owner. Never chmod, recurse,
	 * follow a symlink, or repair a directory belonging to another user. */
	if (info.st_uid == 0 && geteuid() == 0 && fchown(directory, daemon_uid, daemon_gid) != 0)
		goto error;
	if (fstat(directory, &info) != 0 || info.st_uid != daemon_uid || info.st_gid != daemon_gid ||
		(info.st_mode & 07777) != 0700)
		goto error;
	return directory;
error:
	close(directory);
	return -1;
}

static int prepare_alias(int home, const char* name, int activate)
{
	char expected[64];
	char target[64];
	struct stat info;
	if (snprintf(expected, sizeof(expected), "/inspircd/persistent/%s", name) >= (int)sizeof(expected))
		return -1;
	if (fstatat(home, name, &info, AT_SYMLINK_NOFOLLOW) != 0)
	{
		/* An interrupted empty-directory conversion may have removed the old
		 * directory before creating its link. Creation never overwrites an entry. */
		if (errno != ENOENT)
			return -1;
		return activate ? symlinkat(expected, home, name) : 0;
	}
	if (info.st_uid != 0 && info.st_uid != daemon_uid)
		return -1;
	if (S_ISLNK(info.st_mode))
	{
		ssize_t length = readlinkat(home, name, target, sizeof(target));
		return length == (ssize_t)strlen(expected) && memcmp(target, expected, (size_t)length) == 0 ? 0 : -1;
	}
	int fd = openat(home, name, directory_flags);
	if (fd < 0)
		return -1;
	if (safe_parent(fd) != 0)
	{
		close(fd);
		return -1;
	}
	DIR* directory = fdopendir(fd);
	if (directory == NULL)
	{
		close(fd);
		return -1;
	}
	struct dirent* entry;
	int empty = 1;
	errno = 0;
	while ((entry = readdir(directory)) != NULL)
	{
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
		{
			empty = 0;
			break;
		}
	}
	if (errno != 0)
		empty = 0;
	closedir(directory);
	if (!empty)
		return -1;
	/* AT_REMOVEDIR itself refuses symlinks and nonempty directories, even if
	 * the daemon-owned image directory changed since the check above. */
	if (activate && (unlinkat(home, name, AT_REMOVEDIR) != 0 || symlinkat(expected, home, name) != 0))
		return -1;
	return 0;
}

static int prepare_persistent_directories(void)
{
	int status = -1;
	int root = -1;
	int home = -1;
	int volume = -1;
	int children[3] = {-1, -1, -1};
	int databases = -1;
	const char* names[3] = {"conf", "data", "logs"};

	/* Open every ancestor separately, without following symlinks. Descriptors
	 * pin the checked directories for all subsequent privileged operations. */
	root = open("/", directory_flags);
	if (root < 0 || safe_parent(root) != 0)
		goto done;
	home = openat(root, "inspircd", directory_flags);
	if (home < 0 || safe_parent(home) != 0)
		goto done;
	volume = openat(home, "persistent", directory_flags);
	if (volume < 0 || safe_parent(volume) != 0)
		goto done;
	for (size_t i = 0; i < 3; ++i)
	{
		if (prepare_alias(home, names[i], 0) != 0)
			goto done;
	}
	for (size_t i = 0; i < 3; ++i)
	{
		children[i] = private_directory(volume, names[i]);
		if (children[i] < 0)
			goto done;
	}
	databases = private_directory(children[1], "script-databases");
	if (databases < 0)
		goto done;
	for (size_t i = 0; i < 3; ++i)
	{
		if (prepare_alias(home, names[i], 1) != 0)
			goto done;
	}
	status = 0;
done:
	if (databases >= 0)
		close(databases);
	for (size_t i = 0; i < 3; ++i)
	{
		if (children[i] >= 0)
			close(children[i]);
	}
	if (volume >= 0)
		close(volume);
	if (home >= 0)
		close(home);
	if (root >= 0)
		close(root);
	if (status != 0)
		return fail("cannot initialize or validate /inspircd/persistent and its fixed aliases");
	return 0;
}

int main(int argc, char** argv)
{
	(void)argc;
	uid_t real_uid, effective_uid, saved_uid;
	gid_t real_gid, effective_gid, saved_gid;
	/* A launcher with a non-root effective UID can still retain a root real or
	 * saved identity. Reject mixed IDs before opening paths or executing helpers. */
	if (getresuid(&real_uid, &effective_uid, &saved_uid) != 0 ||
		getresgid(&real_gid, &effective_gid, &saved_gid) != 0 || real_uid != effective_uid || real_uid != saved_uid ||
		real_gid != effective_gid || real_gid != saved_gid)
		return fail("mixed or unreadable process identities");
	const char* persistent = getenv("INSPIRCD_PERSISTENT");
	if (persistent != NULL && persistent[0] != '\0' && strcmp(persistent, "no") != 0)
	{
		if (strcmp(persistent, "yes") != 0)
			return fail("INSPIRCD_PERSISTENT must be yes or no");
		if (prepare_persistent_directories() != 0)
			return EXIT_FAILURE;
	}
	/* Also drop root when persistence is disabled. All real, effective and saved IDs
	 * are set, and supplementary groups are cleared before the first exec. */
	if (geteuid() == 0)
	{
		if (setgroups(0, NULL) != 0 || setresgid(daemon_gid, daemon_gid, daemon_gid) != 0 ||
			setresuid(daemon_uid, daemon_uid, daemon_uid) != 0 || getuid() != daemon_uid || geteuid() != daemon_uid ||
			getgid() != daemon_gid || getegid() != daemon_gid || getgroups(0, NULL) != 0)
			return fail("cannot drop root privileges");
	}
	/* Preserve argv boundaries and the sealed token flow. The existing
	 * entrypoint remains responsible for materializing and clearing its token. */
	argv[0] = "/entrypoint.sh";
	execv(argv[0], argv);
	return fail("cannot execute entrypoint");
}
