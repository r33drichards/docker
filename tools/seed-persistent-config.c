/*
 * Non-root, fixed-path initialization of the opt-in persistent configuration.
 *
 * Only flat, regular bundled files are managed. Extra files are never enumerated
 * or read during seeding. The explicitly enabled import gate separately verifies
 * the fixed TLS key/certificate against the approved readiness hash manifest.
 * Metadata stores names and SHA-256 hashes only.
 * Each update has a durable write-ahead journal: old hash, new hash, and name.
 * Recovery recognizes only those exact versions, never adopts edited contents,
 * and commits the manifest before removing the journal. A failed new-file
 * publication cannot overwrite an existing file. Deleted managed files remain
 * deleted. Removed image files remain on disk, but become unmanaged.
 *
 * This helper serializes cooperating starts. Administrators should stop the
 * container before editing configuration; no ordinary filesystem rename can
 * provide a compare-and-swap against an uncooperative concurrent editor.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/sha.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_FILES 512
#define MAX_FILE_BYTES (16U * 1024U * 1024U)
#define MAX_TOTAL_BYTES (32U * 1024U * 1024U)
#define MAX_METADATA_BYTES (256U * 1024U)
#define MANIFEST ".seed-persistent-config.manifest"
#define JOURNAL ".seed-persistent-config.journal"
#define LOCK ".seed-persistent-config.lock"
#define BASELINE "seed-persistent-config.baseline"
#define CONFIG_STAGE ".seed-persistent-config.stage"
#define METADATA_STAGE ".seed-persistent-config.metadata-stage"
#define IMPORT_READY ".migration-ready"
#define IMPORT_HEADER "persistent-import-v1\n"
#define MANIFEST_HEADER "seed-persistent-config-v1\n"
#define JOURNAL_HEADER "seed-persistent-config-journal-v1\n"
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif

struct record
{
	char name[NAME_MAX + 1];
	char hash[65];
};
struct source_file
{
	struct record record;
	unsigned char* bytes;
	size_t length;
	mode_t mode;
};
struct blob
{
	unsigned char* bytes;
	size_t length;
	struct stat stat;
};
static struct record manifest[MAX_FILES];
static size_t manifest_count;
static struct source_file sources[MAX_FILES];
static size_t source_count;
static const uid_t daemon_uid = 10000;
static const gid_t daemon_gid = 10000;
static const unsigned import_timeout_seconds = 1800;
static volatile sig_atomic_t import_interrupted;

static void fail(const char* reason)
{
	/* Reasons are fixed strings: do not expose file names, contents, or env. */
	fprintf(stderr, "Persistent configuration: %s.\n", reason);
	exit(EXIT_FAILURE);
}

static bool same_file(const struct stat* a, const struct stat* b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_mode == b->st_mode && a->st_uid == b->st_uid &&
		a->st_gid == b->st_gid && a->st_nlink == b->st_nlink && a->st_size == b->st_size &&
		a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
		a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

static bool regular_file(const struct stat* s)
{
	return S_ISREG(s->st_mode) && s->st_nlink == 1;
}

static bool safe_name(const char* name)
{
	size_t n = strlen(name);
	if (!n || n > NAME_MAX || name[0] == '.')
		return false;
	for (size_t i = 0; i < n; ++i)
	{
		unsigned char c = (unsigned char)name[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
				c == '-'))
			return false;
	}
	return strcmp(name, "key.pem") && strcmp(name, "cert.pem") && strcmp(name, "dhparams.pem");
}

static bool valid_hash(const char* hash)
{
	if (strlen(hash) != 64)
		return false;
	for (size_t i = 0; i < 64; ++i)
		if (!((hash[i] >= '0' && hash[i] <= '9') || (hash[i] >= 'a' && hash[i] <= 'f')))
			return false;
	return true;
}

static void hash_bytes(const unsigned char* bytes, size_t size, char out[65])
{
	unsigned char hash[SHA256_DIGEST_LENGTH];
	static const char hex[] = "0123456789abcdef";
	if (!SHA256(bytes, size, hash))
		fail("checksum computation failed");
	for (size_t i = 0; i < sizeof(hash); ++i)
	{
		out[2 * i] = hex[hash[i] >> 4];
		out[2 * i + 1] = hex[hash[i] & 15];
	}
	out[64] = '\0';
}

static void check_directory(int fd, bool private)
{
	struct stat s;
	if (fstat(fd, &s) || !S_ISDIR(s.st_mode) || (s.st_uid != 0 && s.st_uid != geteuid()) || (s.st_mode & 0022))
		fail("unsafe directory");
	if (private && (s.st_uid != geteuid() || (s.st_mode & 07777) != 0700))
		fail("configuration directory must be owned by the daemon with mode 0700");
}

static int open_directory(const char* path, bool private)
{
	/* All components, including the fixed mount, must be real directories. */
	int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		fail("cannot open filesystem root");
	check_directory(fd, false);
	char copy[PATH_MAX];
	if (strlen(path) >= sizeof(copy))
		fail("invalid fixed path");
	strcpy(copy, path);
	char* save = NULL;
	for (char* part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save))
	{
		int child = openat(fd, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (child < 0)
			fail("cannot safely open fixed directory");
		close(fd);
		fd = child;
		check_directory(fd, false);
	}
	check_directory(fd, private);
	return fd;
}

static bool file_stat(int dir, const char* name, struct stat* s, bool metadata)
{
	if (fstatat(dir, name, s, AT_SYMLINK_NOFOLLOW))
	{
		if (errno == ENOENT)
			return false;
		fail("cannot inspect configuration entry");
	}
	if (!regular_file(s))
		fail("symlink, hardlink, or non-regular configuration entry refused");
	if (metadata && (s->st_uid != geteuid() || (s->st_mode & 07777) != 0600))
		fail("unsafe configuration metadata");
	return true;
}

static struct blob read_file(int dir, const char* name, size_t limit, bool metadata)
{
	struct blob result = {0};
	int fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0 || fstat(fd, &result.stat) || !regular_file(&result.stat))
		fail("cannot safely read configuration entry");
	if (metadata && (result.stat.st_uid != geteuid() || (result.stat.st_mode & 07777) != 0600))
		fail("unsafe configuration metadata");
	if (result.stat.st_size < 0 || (uintmax_t)result.stat.st_size > limit)
		fail("configuration entry exceeds size limit");
	result.length = (size_t)result.stat.st_size;
	result.bytes = malloc(result.length + 1);
	if (!result.bytes)
		fail("out of memory");
	size_t offset = 0;
	while (offset < result.length)
	{
		ssize_t count = read(fd, result.bytes + offset, result.length - offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			fail("configuration entry changed while reading");
		offset += (size_t)count;
	}
	result.bytes[result.length] = '\0';
	struct stat after, entry;
	if (fstat(fd, &after) || !same_file(&result.stat, &after) || !file_stat(dir, name, &entry, metadata) ||
		!same_file(&entry, &after))
		fail("configuration entry changed while reading");
	close(fd);
	return result;
}

static void interrupt_import(int signal_number)
{
	(void)signal_number;
	import_interrupted = 1;
}

static bool import_requested(void)
{
	const char* value = getenv("INSPIRCD_PERSISTENT_IMPORT");
	if (!value || !*value || !strcmp(value, "no"))
		return false;
	if (strcmp(value, "yes"))
		fail("invalid import setting");
	const char* persistent = getenv("INSPIRCD_PERSISTENT");
	if (!persistent || strcmp(persistent, "yes"))
		fail("import requires persistent mode");
	return true;
}

static void import_file_hash(int dir, const char* name, bool private_key, char hash[65])
{
	struct blob file = read_file(dir, name, MAX_FILE_BYTES, false);
	if (!file.length || file.stat.st_uid != daemon_uid || file.stat.st_gid != daemon_gid ||
		(private_key ? (file.stat.st_mode & 07777) != 0600 : (file.stat.st_mode & 07022) != 0))
		fail("unsafe imported TLS file");
	hash_bytes(file.bytes, file.length, hash);
	free(file.bytes);
}

static void verify_import(int dir)
{
	struct blob marker = read_file(dir, IMPORT_READY, 512, true);
	if (marker.stat.st_uid != daemon_uid || marker.stat.st_gid != daemon_gid)
		fail("unsafe import readiness ownership");
	struct stat config;
	if (!file_stat(dir, "inspircd.conf", &config, false) || !config.st_size || config.st_uid != daemon_uid ||
		config.st_gid != daemon_gid || (config.st_mode & 07022))
		fail("main configuration has not been safely imported");
	/* These explicitly approved fixed TLS files are the only private extras read. */
	char key_hash[65], cert_hash[65];
	import_file_hash(dir, "key.pem", true, key_hash);
	import_file_hash(dir, "cert.pem", false, cert_hash);
	char expected[sizeof(IMPORT_HEADER) + 2 * 64 + sizeof(" key.pem\n") + sizeof(" cert.pem\n")];
	int length = snprintf(expected, sizeof(expected), IMPORT_HEADER "%s key.pem\n%s cert.pem\n", key_hash, cert_hash);
	if (length < 0 || (size_t)length != marker.length || memcmp(marker.bytes, expected, marker.length))
		fail("import readiness does not match the approved TLS files");
	/* Keep this identity pin on every import-enabled start. Intentional TLS
	 * rotation requires replacing the files and then atomically publishing a
	 * matching marker while the container is stopped; never auto-adopt changes. */
	free(marker.bytes);
}

static void wait_for_import(int dir)
{
	struct sigaction handler = {0}, previous_term, previous_int;
	handler.sa_handler = interrupt_import;
	if (sigemptyset(&handler.sa_mask) || sigaction(SIGTERM, &handler, &previous_term) ||
		sigaction(SIGINT, &handler, &previous_int))
		fail("cannot safely wait for import");
	struct timespec started;
	if (clock_gettime(CLOCK_MONOTONIC, &started))
		fail("cannot start import deadline");
	bool announced = false;
	for (;;)
	{
		if (import_interrupted)
			fail("import wait interrupted");
		struct stat marker;
		if (file_stat(dir, IMPORT_READY, &marker, true))
		{
			verify_import(dir);
			break;
		}
		if (!announced)
		{
			fputs("Persistent configuration: waiting for approved TLS import.\n", stderr);
			announced = true;
		}
		struct timespec now;
		if (clock_gettime(CLOCK_MONOTONIC, &now))
			fail("cannot check import deadline");
		if (now.tv_sec - started.tv_sec >= (time_t)import_timeout_seconds)
			fail("import readiness deadline expired");
		struct timespec delay = {.tv_sec = 1, .tv_nsec = 0};
		if (nanosleep(&delay, NULL) && errno != EINTR)
			fail("cannot wait for import");
	}
	if (import_interrupted || sigaction(SIGTERM, &previous_term, NULL) || sigaction(SIGINT, &previous_int, NULL))
		fail("import wait interrupted");
}

static int record_index(const struct record* records, size_t count, const char* name)
{
	for (size_t i = 0; i < count; ++i)
		if (!strcmp(records[i].name, name))
			return (int)i;
	return -1;
}

static int source_index(const char* name)
{
	for (size_t i = 0; i < source_count; ++i)
		if (!strcmp(sources[i].record.name, name))
			return (int)i;
	return -1;
}

static void set_record(const char* name, const char* hash)
{
	int i = record_index(manifest, manifest_count, name);
	if (i < 0)
	{
		if (manifest_count == MAX_FILES)
			fail("too many managed configuration files");
		i = (int)manifest_count++;
		strcpy(manifest[i].name, name);
	}
	strcpy(manifest[i].hash, hash);
}

static int compare_sources(const void* left, const void* right)
{
	const struct source_file *a = left, *b = right;
	return strcmp(a->record.name, b->record.name);
}

static void load_sources(int dir)
{
	int duplicate = dup(dir);
	DIR* listing = duplicate < 0 ? NULL : fdopendir(duplicate);
	if (!listing)
		fail("cannot list bundled configuration");
	size_t total = 0;
	for (;;)
	{
		errno = 0;
		struct dirent* entry = readdir(listing);
		if (!entry)
		{
			if (errno)
				fail("cannot list bundled configuration");
			break;
		}
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		if (!safe_name(entry->d_name) || source_count == MAX_FILES)
			fail("unsupported bundled configuration entry");
		struct blob blob = read_file(dir, entry->d_name, MAX_FILE_BYTES, false);
		if ((blob.stat.st_uid != 0 && blob.stat.st_uid != geteuid()) || (blob.stat.st_mode & 0022))
			fail("unsafe bundled configuration permissions");
		if (blob.length > MAX_TOTAL_BYTES - total)
			fail("bundled configuration exceeds size limit");
		total += blob.length;
		struct source_file* source = &sources[source_count++];
		strcpy(source->record.name, entry->d_name);
		hash_bytes(blob.bytes, blob.length, source->record.hash);
		source->bytes = blob.bytes;
		source->length = blob.length;
		source->mode = (blob.stat.st_mode & 0111) ? 0755 : 0644;
	}
	closedir(listing);
	qsort(sources, source_count, sizeof(sources[0]), compare_sources);
}

static bool load_records(int dir, const char* name, bool metadata)
{
	struct stat s;
	if (!file_stat(dir, name, &s, metadata))
		return false;
	if (!metadata && ((s.st_uid != 0 && s.st_uid != geteuid()) || (s.st_mode & 0022)))
		fail("unsafe bundled baseline");
	struct blob blob = read_file(dir, name, MAX_METADATA_BYTES, metadata);
	size_t header = strlen(MANIFEST_HEADER);
	if (blob.length < header || memcmp(blob.bytes, MANIFEST_HEADER, header) || memchr(blob.bytes, '\0', blob.length))
		fail("invalid configuration manifest");
	char* line = (char*)blob.bytes + header;
	while (*line)
	{
		char* end = strchr(line, '\n');
		if (!end)
			fail("incomplete configuration manifest");
		*end = '\0';
		char* space = strchr(line, ' ');
		if (!space)
			fail("invalid configuration manifest");
		*space++ = '\0';
		if (!valid_hash(line) || !safe_name(space) || record_index(manifest, manifest_count, space) >= 0)
			fail("invalid configuration manifest");
		set_record(space, line);
		line = end + 1;
	}
	free(blob.bytes);
	return true;
}

static void adopt_baseline(int dir)
{
	int root = open_directory("/", false);
	(void)load_records(root, BASELINE, false);
	close(root);
	for (size_t i = 0; i < manifest_count;)
	{
		bool adopt = false;
		struct stat s;
		if (source_index(manifest[i].name) >= 0 && file_stat(dir, manifest[i].name, &s, false))
		{
			struct blob current = read_file(dir, manifest[i].name, MAX_FILE_BYTES, false);
			char hash[65];
			hash_bytes(current.bytes, current.length, hash);
			free(current.bytes);
			adopt = !strcmp(hash, manifest[i].hash);
		}
		if (adopt)
			++i;
		else
			manifest[i] = manifest[--manifest_count];
	}
}

static void sync_directory(int dir)
{
	if (fsync(dir))
		fail("cannot durably save configuration directory");
}

static void write_all(int fd, const unsigned char* bytes, size_t length)
{
	size_t offset = 0;
	while (offset < length)
	{
		ssize_t count = write(fd, bytes + offset, length - offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			fail("cannot write configuration update");
		offset += (size_t)count;
	}
}

static void cleanup_stage(int dir, const char* name, bool metadata)
{
	/* Exactly these two names are reserved helper scratch slots. Never enumerate
	 * user files or delete other similarly prefixed names. A crash can leave at
	 * most one bounded configuration stage and one bounded metadata stage. */
	struct stat s;
	if (!file_stat(dir, name, &s, metadata))
		return;
	if (s.st_uid != geteuid() || s.st_gid != getegid() || (s.st_mode & 07000) || s.st_size < 0 ||
		(uintmax_t)s.st_size > (metadata ? MAX_METADATA_BYTES : MAX_FILE_BYTES))
		fail("unsafe abandoned configuration stage");
	if (unlinkat(dir, name, 0))
		fail("cannot remove abandoned configuration stage");
	sync_directory(dir);
}

static void prepare_file(int dir, const unsigned char* bytes, size_t length, mode_t mode, const char* name)
{
	int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		fail("cannot create configuration update");
	write_all(fd, bytes, length);
	if (fchmod(fd, mode) || fsync(fd))
		fail("cannot durably prepare configuration update");
	if (close(fd))
		fail("cannot close configuration update");
}

static void atomic_metadata(int dir, const char* name, const char* bytes, size_t length)
{
	struct stat s;
	(void)file_stat(dir, name, &s, true);
	prepare_file(dir, (const unsigned char*)bytes, length, 0600, METADATA_STAGE);
	(void)file_stat(dir, name, &s, true);
	if (renameat(dir, METADATA_STAGE, dir, name))
		fail("cannot publish configuration metadata");
	sync_directory(dir);
}

static void save_manifest(int dir)
{
	char* bytes = malloc(MAX_METADATA_BYTES);
	if (!bytes)
		fail("out of memory");
	size_t used = strlen(MANIFEST_HEADER);
	memcpy(bytes, MANIFEST_HEADER, used);
	for (size_t i = 0; i < manifest_count; ++i)
	{
		int count = snprintf(bytes + used, MAX_METADATA_BYTES - used, "%s %s\n", manifest[i].hash, manifest[i].name);
		if (count < 0 || (size_t)count >= MAX_METADATA_BYTES - used)
			fail("configuration manifest exceeds size limit");
		used += (size_t)count;
	}
	atomic_metadata(dir, MANIFEST, bytes, used);
	free(bytes);
}

static void clear_journal(int dir)
{
	struct stat s;
	if (!file_stat(dir, JOURNAL, &s, true) || unlinkat(dir, JOURNAL, 0))
		fail("cannot clear configuration journal");
	sync_directory(dir);
}

static void recover_journal(int dir)
{
	struct stat s;
	if (!file_stat(dir, JOURNAL, &s, true))
		return;
	struct blob blob = read_file(dir, JOURNAL, MAX_METADATA_BYTES, true);
	size_t header = strlen(JOURNAL_HEADER);
	if (blob.length < header || memcmp(blob.bytes, JOURNAL_HEADER, header) || memchr(blob.bytes, '\0', blob.length))
		fail("invalid configuration journal");
	char* old = (char*)blob.bytes + header;
	char* next = strchr(old, ' ');
	if (!next)
		fail("invalid configuration journal");
	*next++ = '\0';
	char* name = strchr(next, ' ');
	if (!name)
		fail("invalid configuration journal");
	*name++ = '\0';
	char* end = strchr(name, '\n');
	if (!end || end[1])
		fail("invalid configuration journal");
	*end = '\0';
	if ((strcmp(old, "-") && !valid_hash(old)) || !valid_hash(next) || !safe_name(name))
		fail("invalid configuration journal");
	int index = record_index(manifest, manifest_count, name);
	bool committed = index >= 0 && !strcmp(manifest[index].hash, next);
	if (!committed && ((index < 0 && strcmp(old, "-")) || (index >= 0 && strcmp(manifest[index].hash, old))))
		fail("configuration journal does not match manifest");
	/* Never inspect a journal target outside the current bundled-file set. */
	if (!committed && source_index(name) >= 0 && file_stat(dir, name, &s, false))
	{
		struct blob current = read_file(dir, name, MAX_FILE_BYTES, false);
		char hash[65];
		hash_bytes(current.bytes, current.length, hash);
		free(current.bytes);
		if (!strcmp(hash, next))
			set_record(name, next);
	}
	save_manifest(dir);
	clear_journal(dir);
	free(blob.bytes);
}

static void save_journal(int dir, const char* name, const char* old, const char* next)
{
	struct stat s;
	if (file_stat(dir, JOURNAL, &s, true))
		fail("unfinished configuration journal");
	char bytes[sizeof(JOURNAL_HEADER) + 2 * 65 + NAME_MAX + 8];
	int size = snprintf(bytes, sizeof(bytes), JOURNAL_HEADER "%s %s %s\n", old, next, name);
	if (size < 0 || (size_t)size >= sizeof(bytes))
		fail("configuration journal exceeds size limit");
	atomic_metadata(dir, JOURNAL, bytes, (size_t)size);
}

static void update_file(int dir, const struct source_file* source)
{
	const char* name = source->record.name;
	int index = record_index(manifest, manifest_count, name);
	struct stat before;
	bool exists = file_stat(dir, name, &before, false);
	if (exists && index < 0)
		return; /* Unmanaged pre-existing file: do not read, adopt, or replace. */
	if (!exists && index >= 0)
		return; /* Preserve an administrator's deletion. */
	mode_t mode = source->mode;
	if (exists)
	{
		struct blob current = read_file(dir, name, MAX_FILE_BYTES, false);
		char hash[65];
		hash_bytes(current.bytes, current.length, hash);
		before = current.stat;
		free(current.bytes);
		if (strcmp(hash, manifest[index].hash))
			return; /* Preserve edits, including across retries. */
		if (!strcmp(hash, source->record.hash))
			return;
		if (before.st_uid != geteuid() || (before.st_mode & 07000))
			fail("unsafe managed configuration ownership or permissions");
		mode = before.st_mode & 0777;
	}
	prepare_file(dir, source->bytes, source->length, mode, CONFIG_STAGE);
	save_journal(dir, name, exists ? manifest[index].hash : "-", source->record.hash);
	/* TEST-FAILPOINT-AFTER-JOURNAL */
	if (exists)
	{
		struct stat current;
		if (!file_stat(dir, name, &current, false) || !same_file(&before, &current))
			fail("managed configuration changed before publication");
		if (renameat(dir, CONFIG_STAGE, dir, name))
			fail("cannot publish managed configuration update");
	}
	else
	{
		if (syscall(SYS_renameat2, dir, CONFIG_STAGE, dir, name, RENAME_NOREPLACE))
			fail("cannot publish new configuration without overwriting");
	}
	sync_directory(dir);
	/* TEST-FAILPOINT-AFTER-PUBLISH */
	set_record(name, source->record.hash);
	save_manifest(dir);
	/* TEST-FAILPOINT-AFTER-MANIFEST */
	clear_journal(dir);
}

int main(int argc, char** argv)
{
	(void)argv;
	if (argc != 1)
		fail("arguments and path overrides are not supported");
	if (getuid() == 0 || geteuid() == 0)
		fail("must run only after dropping root privileges");
	bool importing = import_requested();
	umask(0077);
	int destination = open_directory("/inspircd/persistent/conf", true);
	/* No lock, metadata, config, or certificate writes before this gate. */
	if (importing)
		wait_for_import(destination);
	int source_dir = open_directory("/conf", false);
	load_sources(source_dir);
	close(source_dir);
	int lock = openat(destination, LOCK, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
	struct stat lock_stat, entry_stat;
	if (lock < 0 || fstat(lock, &lock_stat) || !regular_file(&lock_stat) || lock_stat.st_uid != geteuid() ||
		(lock_stat.st_mode & 07777) != 0600 || flock(lock, LOCK_EX) ||
		!file_stat(destination, LOCK, &entry_stat, true) || !same_file(&lock_stat, &entry_stat))
		fail("cannot acquire safe configuration lock");
	cleanup_stage(destination, CONFIG_STAGE, false);
	cleanup_stage(destination, METADATA_STAGE, true);
	bool has_manifest = load_records(destination, MANIFEST, true);
	/* Preflight all managed targets before publishing any bundled changes. */
	for (size_t i = 0; i < source_count; ++i)
		(void)file_stat(destination, sources[i].record.name, &entry_stat, false);
	if (!has_manifest)
	{
		adopt_baseline(destination);
		save_manifest(destination);
	}
	recover_journal(destination);
	for (size_t i = 0; i < source_count; ++i)
		update_file(destination, &sources[i]);
	/* Drop tracking only, never delete files removed from a newer image. */
	for (size_t i = 0; i < manifest_count;)
	{
		if (source_index(manifest[i].name) < 0)
		{
			manifest[i] = manifest[--manifest_count];
		}
		else
		{
			++i;
		}
	}
	save_manifest(destination);
	for (size_t i = 0; i < source_count; ++i)
		free(sources[i].bytes);
	close(lock);
	close(destination);
	return EXIT_SUCCESS;
}
