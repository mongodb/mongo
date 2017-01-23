/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <string.h>
#include <limits.h>
#include <errno.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <wiredtiger_ext.h>
#include "queue.h"

#define	FAIL_FS_GIGABYTE (1024 * 1024 * 1024)

/*
 * A "fail file system", that is, a file system extension that fails when we
 * want it to.  This is only used in test frameworks, this fact allows us
 * to simplify some error paths.
 */
typedef struct {
	WT_FILE_SYSTEM iface;
	/*
	 * WiredTiger performs schema and I/O operations in parallel, all file
	 * system and file handle access must be thread-safe. This extension
	 * uses a single, global file system lock.
	 */
	pthread_rwlock_t lock;                  /* Lock */
	int64_t read_ops;
	int64_t write_ops;
	int64_t allow_reads;
	int64_t allow_writes;
	/* Queue of file handles */
	TAILQ_HEAD(fail_file_handle_qh, fail_file_handle) fileq;
	WT_EXTENSION_API *wtext;                /* Extension functions */
} FAIL_FILE_SYSTEM;

typedef struct fail_file_handle {
	WT_FILE_HANDLE iface;

	/*
	 * Track the system file descriptor for each file.
	 */
	FAIL_FILE_SYSTEM *fail_fs;              /* Enclosing file system */
	TAILQ_ENTRY(fail_file_handle) q;        /* Queue of handles */
	int fd;					/* System file descriptor */
} FAIL_FILE_HANDLE;

static int fail_file_close(WT_FILE_HANDLE *, WT_SESSION *);
static void fail_file_handle_remove(WT_SESSION *, FAIL_FILE_HANDLE *);
static int fail_file_lock(WT_FILE_HANDLE *, WT_SESSION *, bool);
static int fail_file_read(
    WT_FILE_HANDLE *, WT_SESSION *, wt_off_t, size_t, void *);
static int fail_file_size(
    WT_FILE_HANDLE *, WT_SESSION *, wt_off_t *);
static int fail_file_sync(WT_FILE_HANDLE *, WT_SESSION *);
static int fail_file_truncate(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t);
static int fail_file_write(
    WT_FILE_HANDLE *, WT_SESSION *, wt_off_t, size_t, const void *);
static bool fail_fs_arg(
    const char *match, WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *value,
    int64_t *argp);
static int fail_fs_directory_list(WT_FILE_SYSTEM *, WT_SESSION *,
    const char *, const char *, char ***, uint32_t *);
static int fail_fs_directory_list_free(
    WT_FILE_SYSTEM *, WT_SESSION *, char **, uint32_t);
static int fail_fs_exist(WT_FILE_SYSTEM *, WT_SESSION *, const char *, bool *);
static int fail_fs_open(WT_FILE_SYSTEM *, WT_SESSION *,
    const char *, WT_FS_OPEN_FILE_TYPE, uint32_t, WT_FILE_HANDLE **);
static int fail_fs_remove(
    WT_FILE_SYSTEM *, WT_SESSION *, const char *, uint32_t);
static int fail_fs_rename(
    WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, uint32_t);
static int fail_fs_size(
    WT_FILE_SYSTEM *, WT_SESSION *, const char *, wt_off_t *);
static int fail_fs_terminate(WT_FILE_SYSTEM *, WT_SESSION *);

/*
 * We use pthread functions for portable locking.
 * Assert on errors for simplicity.
 */
static void
fail_fs_allocate_lock(pthread_rwlock_t *lockp)
{
	assert(pthread_rwlock_init(lockp, NULL) == 0);
}

static void
fail_fs_destroy_lock(pthread_rwlock_t *lockp)
{
	assert(pthread_rwlock_destroy(lockp) == 0);
}

static void
fail_fs_lock(pthread_rwlock_t *lockp)
{
	assert(pthread_rwlock_wrlock(lockp) == 0);
}

static void
fail_fs_unlock(pthread_rwlock_t *lockp)
{
	assert(pthread_rwlock_unlock(lockp) == 0);
}

/*
 * fail_file_close --
 *	ANSI C close.
 */
static int
fail_file_close(WT_FILE_HANDLE *file_handle, WT_SESSION *session)
{
	FAIL_FILE_HANDLE *fail_fh;
	int ret;

	(void)session;						/* Unused */

	fail_fh = (FAIL_FILE_HANDLE *)file_handle;

	if (fail_fh->fd < 0)
		return (EINVAL);
	ret = close(fail_fh->fd);
	fail_fh->fd = -1;
	fail_file_handle_remove(session, fail_fh);
	return (ret);
}

/*
 * fail_file_handle_remove --
 *	Destroy an in-memory file handle. Should only happen on remove or
 *	shutdown.
 */
static void
fail_file_handle_remove(WT_SESSION *session, FAIL_FILE_HANDLE *fail_fh)
{
	FAIL_FILE_SYSTEM *fail_fs;

	(void)session;						/* Unused */
	fail_fs = fail_fh->fail_fs;

	TAILQ_REMOVE(&fail_fs->fileq, fail_fh, q);

	free(fail_fh->iface.name);
	free(fail_fh);
}

/*
 * fail_file_lock --
 *	Lock/unlock a file.
 */
static int
fail_file_lock(WT_FILE_HANDLE *file_handle, WT_SESSION *session, bool lock)
{
	/* Locks are always granted. */
	(void)file_handle;					/* Unused */
	(void)session;						/* Unused */
	(void)lock;						/* Unused */

	return (0);
}

/*
 * fail_file_read --
 *	POSIX pread.
 */
static int
fail_file_read(WT_FILE_HANDLE *file_handle,
    WT_SESSION *session, wt_off_t offset, size_t len, void *buf)
{
	FAIL_FILE_HANDLE *fail_fh;
	FAIL_FILE_SYSTEM *fail_fs;
	WT_EXTENSION_API *wtext;
	int64_t read_ops;
	int ret;
	size_t chunk;
	ssize_t nr;
	uint8_t *addr;

	fail_fh = (FAIL_FILE_HANDLE *)file_handle;
	fail_fs = fail_fh->fail_fs;
	wtext = fail_fs->wtext;
	ret = 0;

	fail_fs_lock(&fail_fs->lock);
	read_ops = ++fail_fs->read_ops;
	fail_fs_unlock(&fail_fs->lock);

	if (fail_fs->allow_reads != 0 && read_ops % fail_fs->allow_reads == 0) {
		(void)wtext->msg_printf(wtext, session,
		    "fail_fs: %s: simulated failure after %" PRId64
		    " reads\n", fail_fh->iface.name, read_ops);
		return (EIO);
	}

	for (addr = buf; len > 0; addr += nr, len -= (size_t)nr, offset += nr) {
		chunk = (len < FAIL_FS_GIGABYTE) ? len : FAIL_FS_GIGABYTE;
		if ((nr = pread(fail_fh->fd, addr, chunk, offset)) <= 0) {
			(void)wtext->err_printf(wtext, session,
			    "%s: handle-read: failed to read %" PRIu64
			    " bytes at offset %" PRIu64 ": %s",
			    fail_fh->iface.name, (uint64_t)len,
			    (uint64_t)offset, wtext->strerror(wtext, NULL, nr));
			ret = (nr == 0 ? WT_ERROR : errno);
			break;
		}
	}
	return (ret);
}

/*
 * fail_file_size --
 *	Get the size of a file in bytes, by file handle.
 */
static int
fail_file_size(
    WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t *sizep)
{
	FAIL_FILE_HANDLE *fail_fh;
	struct stat statbuf;
	int ret;

	(void)session;						/* Unused */

	fail_fh = (FAIL_FILE_HANDLE *)file_handle;
	ret = 0;

	if ((ret = fstat(fail_fh->fd, &statbuf)) != 0)
		return (ret);
	*sizep = statbuf.st_size;
	return (0);
}

/*
 * fail_file_sync --
 *	Ensure the content of the file is stable. This is a no-op in our
 *	memory backed file system.
 */
static int
fail_file_sync(WT_FILE_HANDLE *file_handle, WT_SESSION *session)
{
	(void)file_handle;					/* Unused */
	(void)session;						/* Unused */

	return (0);
}

/*
 * fail_file_truncate --
 *	POSIX ftruncate.
 */
static int
fail_file_truncate(
    WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t offset)
{
	FAIL_FILE_HANDLE *fail_fh;

	(void)session;						/* Unused */

	fail_fh = (FAIL_FILE_HANDLE *)file_handle;
	return (ftruncate(fail_fh->fd, offset));
}

/*
 * fail_file_write --
 *	POSIX pwrite.
 */
static int
fail_file_write(WT_FILE_HANDLE *file_handle, WT_SESSION *session,
    wt_off_t offset, size_t len, const void *buf)
{
	FAIL_FILE_HANDLE *fail_fh;
	FAIL_FILE_SYSTEM *fail_fs;
	WT_EXTENSION_API *wtext;
	int64_t write_ops;
	int ret;
	size_t chunk;
	ssize_t nr;
	const uint8_t *addr;

	fail_fh = (FAIL_FILE_HANDLE *)file_handle;
	fail_fs = fail_fh->fail_fs;
	wtext = fail_fs->wtext;
	ret = 0;

	fail_fs_lock(&fail_fs->lock);
	write_ops = ++fail_fs->write_ops;
	fail_fs_unlock(&fail_fs->lock);

	if (fail_fs->allow_writes != 0 &&
	    write_ops % fail_fs->allow_writes == 0) {
		(void)wtext->msg_printf(wtext, session,
		    "fail_fs: %s: simulated failure after %" PRId64
		    " writes\n", fail_fh->iface.name, write_ops);
		return (EIO);
	}

	/* Break writes larger than 1GB into 1GB chunks. */
	for (addr = buf; len > 0; addr += nr, len -= (size_t)nr, offset += nr) {
		chunk = (len < FAIL_FS_GIGABYTE) ? len : FAIL_FS_GIGABYTE;
		if ((nr = pwrite(fail_fh->fd, addr, chunk, offset)) <= 0) {
			(void)wtext->err_printf(wtext, session,
			    "%s: handle-write: failed to write %" PRIu64
			    " bytes at offset %" PRIu64 ": %s",
			    fail_fh->iface.name, (uint64_t)len,
			    (uint64_t)offset, wtext->strerror(wtext, NULL, nr));
			ret = (nr == 0 ? WT_ERROR : errno);
			break;
		}
	}
	return (ret);
}

/*
 * fail_fs_arg --
 *      If the key matches, return the value interpreted as an integer.
 */
static bool
fail_fs_arg(const char *match, WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *value,
    int64_t *argp)
{
	char *s;
	int64_t result;

	if (strncmp(match, key->str, key->len) == 0 &&
	    match[key->len] == '\0') {
		s = (char *)value->str;
		result = strtoll(s, &s, 10);
		if ((size_t)(s - (char *)value->str) == value->len) {
			*argp = result;
			return (true);
		}
	}
	return (false);
}

/*
 * fail_fs_directory_list --
 *	Return a list of files in a given sub-directory.
 */
static int
fail_fs_directory_list(WT_FILE_SYSTEM *file_system,
    WT_SESSION *session, const char *directory,
    const char *prefix, char ***dirlistp, uint32_t *countp)
{
	FAIL_FILE_HANDLE *fail_fh;
	FAIL_FILE_SYSTEM *fail_fs;
	size_t len, prefix_len;
	uint32_t allocated, count;
	int ret;
	char *name, **entries;

	(void)session;						/* Unused */

	fail_fs = (FAIL_FILE_SYSTEM *)file_system;
	ret = 0;
	*dirlistp = NULL;
	*countp = 0;

	entries = NULL;
	allocated = count = 0;
	len = strlen(directory);
	prefix_len = prefix == NULL ? 0 : strlen(prefix);

	fail_fs_lock(&fail_fs->lock);
	TAILQ_FOREACH(fail_fh, &fail_fs->fileq, q) {
		name = fail_fh->iface.name;
		if (strncmp(name, directory, len) != 0 ||
		    (prefix != NULL && strncmp(name, prefix, prefix_len) != 0))
			continue;

		/*
		 * Increase the list size in groups of 10, it doesn't
		 * matter if the list is a bit longer than necessary.
		 */
		if (count >= allocated) {
			entries = realloc(
			    entries, (allocated + 10) * sizeof(char *));
			if (entries == NULL) {
				ret = ENOMEM;
				goto err;
			}
			memset(entries + allocated * sizeof(char *),
			    0, 10 * sizeof(char *));
			allocated += 10;
		}
		entries[count++] = strdup(name);
	}

	*dirlistp = entries;
	*countp = count;

err:	fail_fs_unlock(&fail_fs->lock);
	if (ret == 0)
		return (0);

	if (entries != NULL) {
		while (count > 0)
			free(entries[--count]);
		free(entries);
	}

	return (ret);
}

/*
 * fail_fs_directory_list_free --
 *	Free memory allocated by fail_fs_directory_list.
 */
static int
fail_fs_directory_list_free(WT_FILE_SYSTEM *file_system,
    WT_SESSION *session, char **dirlist, uint32_t count)
{
	(void)file_system;					/* Unused */
	(void)session;						/* Unused */

	if (dirlist != NULL) {
		while (count > 0)
			free(dirlist[--count]);
		free(dirlist);
	}
	return (0);
}

/*
 * fail_fs_exist --
 *	Return if the file exists.
 */
static int
fail_fs_exist(WT_FILE_SYSTEM *file_system,
    WT_SESSION *session, const char *name, bool *existp)
{
	(void)file_system;					/* Unused */
	(void)session;						/* Unused */

	*existp = (access(name, 0) == 0);
	return (0);
}

/*
 * fail_fs_open --
 *	fopen for the fail file system.
 */
static int
fail_fs_open(WT_FILE_SYSTEM *file_system, WT_SESSION *session,
    const char *name, WT_FS_OPEN_FILE_TYPE file_type, uint32_t flags,
    WT_FILE_HANDLE **file_handlep)
{
	FAIL_FILE_HANDLE *fail_fh;
	FAIL_FILE_SYSTEM *fail_fs;
	WT_FILE_HANDLE *file_handle;
	int open_flags;
	int ret;

	(void)file_type;					/* Unused */
	(void)session;						/* Unused */

	*file_handlep = NULL;
	ret = 0;
	fail_fs = (FAIL_FILE_SYSTEM *)file_system;
	fail_fh = NULL;

	fail_fs_lock(&fail_fs->lock);

	open_flags = 0;
	if ((flags & WT_FS_OPEN_CREATE) != 0)
		open_flags |= O_CREAT;
	if ((flags & WT_FS_OPEN_EXCLUSIVE) != 0)
		open_flags |= O_EXCL;
	if ((flags & WT_FS_OPEN_READONLY) != 0)
		open_flags |= O_RDONLY;
	else
		open_flags |= O_RDWR;

	if ((ret = open(name, open_flags, 0666)) < 0)
		goto err;

	/* We create a handle structure for each open. */
	if ((fail_fh = calloc(1, sizeof(FAIL_FILE_HANDLE))) == NULL) {
		ret = ENOMEM;
		goto err;
	}

	/* Initialize private information. */
	fail_fh->fail_fs = fail_fs;
	fail_fh->fd = ret;
	ret = 0;

	/* Initialize public information. */
	file_handle = (WT_FILE_HANDLE *)fail_fh;
	if ((file_handle->name = strdup(name)) == NULL) {
		ret = ENOMEM;
		goto err;
	}

	/* Setup the function call table. */
	file_handle->close = fail_file_close;
	file_handle->fh_advise = NULL;
	file_handle->fh_extend = NULL;
	file_handle->fh_extend_nolock = NULL;
	file_handle->fh_lock = fail_file_lock;
	file_handle->fh_map = NULL;
	file_handle->fh_map_discard = NULL;
	file_handle->fh_map_preload = NULL;
	file_handle->fh_unmap = NULL;
	file_handle->fh_read = fail_file_read;
	file_handle->fh_size = fail_file_size;
	file_handle->fh_sync = fail_file_sync;
	file_handle->fh_sync_nowait = NULL;
	file_handle->fh_truncate = fail_file_truncate;
	file_handle->fh_write = fail_file_write;

	TAILQ_INSERT_HEAD(&fail_fs->fileq, fail_fh, q);

	*file_handlep = file_handle;

	if (0) {
err:		free(fail_fh);
	}

	fail_fs_unlock(&fail_fs->lock);
	return (ret);
}

/*
 * fail_fs_remove --
 *	POSIX remove.
 */
static int
fail_fs_remove(WT_FILE_SYSTEM *file_system,
    WT_SESSION *session, const char *name, uint32_t flags)
{
	(void)file_system;					/* Unused */
	(void)session;						/* Unused */
	(void)flags;						/* Unused */

	return (unlink(name));
}

/*
 * fail_fs_rename --
 *	POSIX rename.
 */
static int
fail_fs_rename(WT_FILE_SYSTEM *file_system,
    WT_SESSION *session, const char *from, const char *to, uint32_t flags)
{
	(void)file_system;					/* Unused */
	(void)session;						/* Unused */
	(void)flags;						/* Unused */

	return (rename(from, to));
}

/*
 * fail_fs_size --
 *	Get the size of a file in bytes, by file name.
 */
static int
fail_fs_size(WT_FILE_SYSTEM *file_system,
    WT_SESSION *session, const char *name, wt_off_t *sizep)
{
	struct stat statbuf;
	int ret;

	(void)file_system;					/* Unused */
	(void)session;						/* Unused */

	ret = 0;
	if ((ret = stat(name, &statbuf)) != 0)
		return (ret);
	*sizep = statbuf.st_size;
	return (0);
}

/*
 * fail_fs_terminate --
 *	Discard any resources on termination
 */
static int
fail_fs_terminate(WT_FILE_SYSTEM *file_system, WT_SESSION *session)
{
	FAIL_FILE_HANDLE *fail_fh;
	FAIL_FILE_SYSTEM *fail_fs;

	fail_fs = (FAIL_FILE_SYSTEM *)file_system;

	while ((fail_fh = TAILQ_FIRST(&fail_fs->fileq)) != NULL)
		fail_file_handle_remove(session, fail_fh);

	fail_fs_destroy_lock(&fail_fs->lock);
	free(fail_fs);

	return (0);
}

/*
 * wiredtiger_extension_init --
 *	WiredTiger fail filesystem extension.
 */
int
wiredtiger_extension_init(WT_CONNECTION *conn, WT_CONFIG_ARG *config)
{
	FAIL_FILE_SYSTEM *fail_fs;
	WT_CONFIG_ITEM k, v;
	WT_CONFIG_PARSER *config_parser;
	WT_EXTENSION_API *wtext;
	WT_FILE_SYSTEM *file_system;
	int ret;

	ret = 0;
	wtext = conn->get_extension_api(conn);
	if ((fail_fs = calloc(1, sizeof(FAIL_FILE_SYSTEM))) == NULL) {
		(void)wtext->err_printf(wtext, NULL,
		    "fail_file_system extension_init: %s",
		    wtext->strerror(wtext, NULL, ENOMEM));
		return (ENOMEM);
	}
	fail_fs->wtext = wtext;
	file_system = (WT_FILE_SYSTEM *)fail_fs;

	/* Get any configuration values. */
	if ((ret = wtext->config_parser_open_arg(
	    wtext, NULL, config, &config_parser)) != 0) {
		(void)wtext->err_printf(wtext, NULL,
		    "WT_EXTENSION_API.config_parser_open: config: %s",
		    wtext->strerror(wtext, NULL, ret));
		goto err;
	}
	while ((ret = config_parser->next(config_parser, &k, &v)) == 0) {
		if (fail_fs_arg("allow_writes", &k, &v, &fail_fs->allow_writes))
			continue;
		if (fail_fs_arg("allow_reads", &k, &v, &fail_fs->allow_reads))
			continue;

		(void)wtext->err_printf(wtext, NULL,
		    "WT_CONFIG_PARSER.next: unexpected configuration "
		    "information: %.*s=%.*s: %s",
		    (int)k.len, k.str, (int)v.len, v.str,
		    wtext->strerror(wtext, NULL, ret));
		goto err;
	}
	if (ret != WT_NOTFOUND) {
		(void)wtext->err_printf(wtext, NULL,
		    "WT_CONFIG_PARSER.next: config: %s",
		    wtext->strerror(wtext, NULL, ret));
		goto err;
	}
	if ((ret = config_parser->close(config_parser)) != 0) {
		(void)wtext->err_printf(wtext, NULL,
		    "WT_CONFIG_PARSER.close: config: %s",
		    wtext->strerror(wtext, NULL, ret));
		goto err;
	}

	fail_fs_allocate_lock(&fail_fs->lock);
	/* Initialize the in-memory jump table. */
	file_system->fs_directory_list = fail_fs_directory_list;
	file_system->fs_directory_list_free = fail_fs_directory_list_free;
	file_system->fs_exist = fail_fs_exist;
	file_system->fs_open_file = fail_fs_open;
	file_system->fs_remove = fail_fs_remove;
	file_system->fs_rename = fail_fs_rename;
	file_system->fs_size = fail_fs_size;
	file_system->terminate = fail_fs_terminate;
	if ((ret = conn->set_file_system(conn, file_system, NULL)) != 0) {
		(void)wtext->err_printf(wtext, NULL,
		    "WT_CONNECTION.set_file_system: %s",
		    wtext->strerror(wtext, NULL, ret));
		goto err;
	}
	return (0);

err:    free(fail_fs);
	return (ret);
}
