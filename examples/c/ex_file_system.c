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
 *
 * ex_file_system.c
 * 	demonstrates how to use the custom file system interface
 */
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <wiredtiger.h>
#include "queue_example.h"

static const char *home;

/*
 * Example file system implementation. Using memory buffers to represent files.
 *
 * WARNING: This implementation isn't thread safe: WiredTiger performs schema
 * and I/O operations in parallel, so all access to the handle must be thread-
 * safe.
 */
typedef struct {
	WT_FILE_SYSTEM iface;

	int opened_file_count;
	int opened_unique_file_count;
	int closed_file_count;

	/* Queue of file handles */
	TAILQ_HEAD(demo_file_handle_qh, demo_file_handle) fileq;

} DEMO_FILE_SYSTEM;

typedef struct demo_file_handle {
	WT_FILE_HANDLE iface;

	/*
	 * Add custom file handle fields after the interface.
	 */
	DEMO_FILE_SYSTEM *demo_fs;

	TAILQ_ENTRY(demo_file_handle) q;
	uint32_t ref;				/* Reference count */

	char	*buf;				/* In-memory contents */
	size_t	 size;
	size_t	 off;				/* Read/write offset */
} DEMO_FILE_HANDLE;

/*
 * Extension initialization function.
 */
#ifdef _WIN32
/*
 * Explicitly export this function so it is visible when loading extensions.
 */
__declspec(dllexport)
#endif
int demo_file_system_create(WT_CONNECTION *, WT_CONFIG_ARG *);

/*
 * Forward function declarations for file system API implementation
 */
static int demo_fs_open(WT_FILE_SYSTEM *,
    WT_SESSION *, const char *, WT_OPEN_FILE_TYPE, uint32_t, WT_FILE_HANDLE **);
static int demo_fs_directory_list(WT_FILE_SYSTEM *, WT_SESSION *,
    const char *, const char *, char ***, uint32_t *);
static int demo_fs_directory_list_free(
    WT_FILE_SYSTEM *, WT_SESSION *, char **, uint32_t);
static int demo_fs_directory_sync(WT_FILE_SYSTEM *file_system,
    WT_SESSION *session, const char *directory);
static int demo_fs_exist(WT_FILE_SYSTEM *, WT_SESSION *, const char *, bool *);
static int demo_fs_remove(WT_FILE_SYSTEM *, WT_SESSION *, const char *);
static int demo_fs_rename(
    WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *);
static int demo_fs_size(
    WT_FILE_SYSTEM *, WT_SESSION *, const char *, wt_off_t *);
static int demo_fs_terminate(WT_FILE_SYSTEM *, WT_SESSION *);

/*
 * Forward function declarations for file handle API implementation
 */
static int demo_file_close(WT_FILE_HANDLE *, WT_SESSION *);
static int demo_file_lock(WT_FILE_HANDLE *, WT_SESSION *, bool);
static int demo_file_read(
    WT_FILE_HANDLE *, WT_SESSION *, wt_off_t, size_t, void *);
static int demo_file_size(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t *);
static int demo_file_sync(WT_FILE_HANDLE *, WT_SESSION *);
static int demo_file_sync_nowait(WT_FILE_HANDLE *, WT_SESSION *);
static int demo_file_truncate(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t);
static int demo_file_write(
    WT_FILE_HANDLE *, WT_SESSION *, wt_off_t, size_t, const void *);

/*
 * Forward function declarations for internal functions
 */
static int demo_handle_remove(WT_SESSION *, DEMO_FILE_HANDLE *);
static DEMO_FILE_HANDLE *demo_handle_search(WT_FILE_SYSTEM *, const char *);

#define	DEMO_FILE_SIZE_INCREMENT	32768

/*
 * demo_file_system_create --
 *	Initialization point for demo file system
 */
int
demo_file_system_create(WT_CONNECTION *conn, WT_CONFIG_ARG *config)
{
	WT_FILE_SYSTEM *file_system;
	DEMO_FILE_SYSTEM *demo_fs;
	int ret = 0;

	(void)config;						/* Unused */

	if ((demo_fs = calloc(1, sizeof(DEMO_FILE_SYSTEM))) == NULL)
		return (ENOMEM);
	file_system = (WT_FILE_SYSTEM *)demo_fs;

	/* Initialize the in-memory jump table. */
	file_system->directory_list = demo_fs_directory_list;
	file_system->directory_list_free = demo_fs_directory_list_free;
	file_system->directory_sync = demo_fs_directory_sync;
	file_system->exist = demo_fs_exist;
	file_system->open_file = demo_fs_open;
	file_system->remove = demo_fs_remove;
	file_system->rename = demo_fs_rename;
	file_system->size = demo_fs_size;
	file_system->terminate = demo_fs_terminate;

	if ((ret = conn->set_file_system(conn, file_system, NULL)) != 0) {
		fprintf(stderr, "Error setting custom file system: %s\n",
		    wiredtiger_strerror(ret));
		goto err;
	}

	return (0);

err:	free(demo_fs);
	/* An error installing the file system is fatal. */
	exit(1);
}

/*
 * demo_fs_open --
 *	fopen for our demo file system
 */
static int
demo_fs_open(WT_FILE_SYSTEM *file_system, WT_SESSION *session,
    const char *name, WT_OPEN_FILE_TYPE file_type, uint32_t flags,
    WT_FILE_HANDLE **file_handlep)
{
	WT_FILE_HANDLE *file_handle;
	DEMO_FILE_HANDLE *demo_fh;
	DEMO_FILE_SYSTEM *demo_fs;

	(void)file_type;					/* Unused */
	(void)session;						/* Unused */
	(void)flags;						/* Unused */

	demo_fs = (DEMO_FILE_SYSTEM *)file_system;
	demo_fh = NULL;

	++demo_fs->opened_file_count;

	/*
	 * First search the file queue, if we find it, assert there's only a
	 * single reference, we only supports a single handle on any file.
	 */
	demo_fh = demo_handle_search(file_system, name);
	if (demo_fh != NULL) {
		if (demo_fh->ref != 0) {
			fprintf(stderr,
			    "demo_file_open of already open file %s\n",
			    name);
			return (EBUSY);
		}

		demo_fh->ref = 1;
		demo_fh->off = 0;

		*file_handlep = (WT_FILE_HANDLE *)demo_fh;
		return (0);
	}

	/* The file hasn't been opened before, create a new one. */
	if ((demo_fh = calloc(1, sizeof(DEMO_FILE_HANDLE))) == NULL)
		return (ENOMEM);

	/* Initialize private information. */
	demo_fh->ref = 1;
	demo_fh->off = 0;
	demo_fh->demo_fs = demo_fs;
	if ((demo_fh->buf = calloc(1, DEMO_FILE_SIZE_INCREMENT)) == NULL)
		goto enomem;
	demo_fh->size = DEMO_FILE_SIZE_INCREMENT;

	/* Initialize public information. */
	file_handle = (WT_FILE_HANDLE *)demo_fh;
	if ((file_handle->name = strdup(name)) == NULL)
		goto enomem;

	/*
	 * Setup the function call table for our custom file system. Set the
	 * function pointer to NULL where our implementation doesn't support
	 * the functionality.
	 */
	file_handle->close = demo_file_close;
	file_handle->fadvise = NULL;
	file_handle->fallocate = NULL;
	file_handle->fallocate_nolock = NULL;
	file_handle->lock = demo_file_lock;
	file_handle->map = NULL;
	file_handle->map_discard = NULL;
	file_handle->map_preload = NULL;
	file_handle->unmap = NULL;
	file_handle->read = demo_file_read;
	file_handle->size = demo_file_size;
	file_handle->sync = demo_file_sync;
	file_handle->sync_nowait = demo_file_sync_nowait;
	file_handle->truncate = demo_file_truncate;
	file_handle->write = demo_file_write;

	TAILQ_INSERT_HEAD(&demo_fs->fileq, demo_fh, q);
	++demo_fs->opened_unique_file_count;

	*file_handlep = file_handle;
	return (0);

enomem:	free(demo_fh->buf);
	free(demo_fh);
	return (ENOMEM);
}

/*
 * demo_fs_directory_list --
 *	Return a list of files in a given sub-directory.
 */
static int
demo_fs_directory_list(WT_FILE_SYSTEM *file_system,
    WT_SESSION *session, const char *directory,
    const char *prefix, char ***dirlistp, uint32_t *countp)
{
	DEMO_FILE_HANDLE *demo_fh;
	DEMO_FILE_SYSTEM *demo_fs;
	size_t dir_len, prefix_len;
	char *name, **entries;
	uint32_t allocated, count;

	(void)session;						/* Unused */

	demo_fs = (DEMO_FILE_SYSTEM *)file_system;
	entries = NULL;
	allocated = count = 0;
	dir_len = strlen(directory);
	prefix_len = prefix == NULL ? 0 : strlen(prefix);

	TAILQ_FOREACH(demo_fh, &demo_fs->fileq, q) {
		name = demo_fh->iface.name;
		if (strncmp(name, directory, dir_len) != 0 ||
		    (prefix != NULL && strncmp(name, prefix, prefix_len) != 0))
			continue;

		/*
		 * Increase the list size in groups of 10, it doesn't
		 * matter if the list is a bit longer than necessary.
		 */
		if (count >= allocated) {
			entries = realloc(
			    entries, (allocated + 10) * sizeof(char *));
			if (entries == NULL)
				return (ENOMEM);
			memset(entries + allocated * sizeof(char *),
			    0, 10 * sizeof(char *));
			allocated += 10;
		}
		entries[count++] = strdup(name);
	}

	*dirlistp = entries;
	*countp = count;

	return (0);
}

/*
 * demo_fs_directory_list_free --
 *	Free memory allocated by demo_fs_directory_list.
 */
static int
demo_fs_directory_list_free(WT_FILE_SYSTEM *file_system,
    WT_SESSION *session, char **dirlist, uint32_t count)
{
	(void)file_system;
	(void)session;

	if (dirlist != NULL) {
		while (count > 0)
			free(dirlist[--count]);
		free(dirlist);
	}
	return (0);
}

/*
 * demo_fs_directory_sync --
 *	Directory sync for our demo file system, which is a no-op.
 */
static int
demo_fs_directory_sync(WT_FILE_SYSTEM *file_system,
    WT_SESSION *session, const char *directory)
{
	(void)file_system;		/* Unused */
	(void)session;			/* Unused */
	(void)directory;		/* Unused */

	return (0);
}

/*
 * demo_fs_exist --
 *	Return if the file exists.
 */
static int
demo_fs_exist(WT_FILE_SYSTEM *file_system,
    WT_SESSION *session, const char *name, bool *existp)
{
	(void)session;						/* Unused */

	*existp =
	    demo_handle_search(file_system, name) != NULL;

	return (0);
}

/*
 * demo_fs_remove --
 *	POSIX remove.
 */
static int
demo_fs_remove(
    WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name)
{
	DEMO_FILE_HANDLE *demo_fh;
	int ret;

	ret = ENOENT;
	if ((demo_fh = demo_handle_search(file_system, name)) != NULL)
		ret = demo_handle_remove(session, demo_fh);

	return (ret);
}

/*
 * demo_fs_rename --
 *	POSIX rename.
 */
static int
demo_fs_rename(WT_FILE_SYSTEM *file_system,
    WT_SESSION *session, const char *from, const char *to)
{
	DEMO_FILE_HANDLE *demo_fh;
	char *copy;

	(void)session;						/* Unused */

	if ((demo_fh = demo_handle_search(file_system, from)) == NULL)
		return (ENOENT);

	if ((copy = strdup(to)) == NULL)
		return (ENOMEM);

	free(demo_fh->iface.name);
	demo_fh->iface.name = copy;
	return (0);
}

/*
 * demo_fs_size --
 *	Get the size of a file in bytes, by file name.
 */
static int
demo_fs_size(WT_FILE_SYSTEM *file_system,
    WT_SESSION *session, const char *name, wt_off_t *sizep)
{
	DEMO_FILE_HANDLE *demo_fh;
	int ret = 0;

	ret = ENOENT;
	if ((demo_fh = demo_handle_search(file_system, name)) != NULL)
		ret = demo_file_size(
		    (WT_FILE_HANDLE *)demo_fh, session, sizep);

	return (ret);
}

/*
 * demo_fs_terminate --
 *	Discard any resources on termination
 */
static int
demo_fs_terminate(WT_FILE_SYSTEM *file_system, WT_SESSION *session)
{
	DEMO_FILE_HANDLE *demo_fh;
	DEMO_FILE_SYSTEM *demo_fs;
	int ret = 0, tret;

	(void)session;						/* Unused */

	demo_fs = (DEMO_FILE_SYSTEM *)file_system;

	while ((demo_fh = TAILQ_FIRST(&demo_fs->fileq)) != NULL)
		if ((tret =
		    demo_handle_remove(session, demo_fh)) != 0 && ret == 0)
			ret = tret;

	printf("Custom file system\n");
	printf("\t%d unique file opens\n", demo_fs->opened_unique_file_count);
	printf("\t%d opened\n", demo_fs->opened_file_count);
	printf("\t%d closed\n", demo_fs->closed_file_count);
	free(demo_fs);

	return (ret);
}

/*
 * demo_file_close --
 *	ANSI C close.
 */
static int
demo_file_close(WT_FILE_HANDLE *file_handle, WT_SESSION *session)
{
	DEMO_FILE_HANDLE *demo_fh;

	(void)session;						/* Unused */

	demo_fh = (DEMO_FILE_HANDLE *)file_handle;
	if (demo_fh->ref < 1) {
		fprintf(stderr, "Closing already closed handle: %s\n",
		    demo_fh->iface.name);
		return (EINVAL);
	}
	--demo_fh->ref;

	if (demo_fh->ref == 0)
		++demo_fh->demo_fs->closed_file_count;

	return (0);
}

/*
 * demo_file_lock --
 *	Lock/unlock a file.
 */
static int
demo_file_lock(WT_FILE_HANDLE *file_handle, WT_SESSION *session, bool lock)
{
	/* Locks are always granted. */
	(void)file_handle;					/* Unused */
	(void)session;						/* Unused */
	(void)lock;						/* Unused */
	return (0);
}

/*
 * demo_file_read --
 *	POSIX pread.
 */
static int
demo_file_read(WT_FILE_HANDLE *file_handle,
    WT_SESSION *session, wt_off_t offset, size_t len, void *buf)
{
	DEMO_FILE_HANDLE *demo_fh;
	int ret = 0;
	size_t off;

	(void)session;						/* Unused */
	demo_fh = (DEMO_FILE_HANDLE *)file_handle;

	off = (size_t)offset;
	if (off < demo_fh->size) {
		if (len > demo_fh->size - off)
			len = demo_fh->size - off;
		memcpy(buf, (uint8_t *)demo_fh->buf + off, len);
		demo_fh->off = off + len;
	} else
		ret = EINVAL;

	if (ret == 0)
		return (0);
	/*
	 * WiredTiger should never request data past the end of a file, so
	 * flag an error if it does.
	 */
	fprintf(stderr,
	    "%s: handle-read: failed to read %zu bytes at offset %zu\n",
	    demo_fh->iface.name, len, off);
	return (EINVAL);
}

/*
 * demo_file_size --
 *	Get the size of a file in bytes, by file handle.
 */
static int
demo_file_size(
    WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t *sizep)
{
	DEMO_FILE_HANDLE *demo_fh;

	(void)session;						/* Unused */
	demo_fh = (DEMO_FILE_HANDLE *)file_handle;

	assert(demo_fh->size != 0);
	*sizep = (wt_off_t)demo_fh->size;
	return (0);
}

/*
 * demo_file_sync --
 *	Ensure the content of the file is stable. This is a no-op in our
 *	memory backed file system.
 */
static int
demo_file_sync(WT_FILE_HANDLE *file_handle, WT_SESSION *session)
{
	(void)file_handle;					/* Unused */
	(void)session;						/* Unused */

	return (0);
}

/*
 * demo_file_sync_nowait --
 *	Ensure the content of the file is stable. This is a no-op in our
 *	memory backed file system.
 */
static int
demo_file_sync_nowait(WT_FILE_HANDLE *file_handle, WT_SESSION *session)
{
	(void)file_handle;					/* Unused */
	(void)session;						/* Unused */

	return (0);
}

/*
 * demo_file_truncate --
 *	POSIX ftruncate.
 */
static int
demo_file_truncate(
    WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t offset)
{
	DEMO_FILE_HANDLE *demo_fh;
	size_t off;

	(void)session;						/* Unused */
	demo_fh = (DEMO_FILE_HANDLE *)file_handle;

	/*
	 * Grow the buffer as necessary, clear any new space in the file,
	 * and reset the file's data length.
	 */
	off = (size_t)offset;
	demo_fh->buf = realloc(demo_fh->buf, off);
	if (demo_fh->buf == NULL) {
		fprintf(stderr, "Failed to resize buffer in truncate\n");
		return (ENOSPC);
	}
	if (demo_fh->size < off)
		memset((uint8_t *)demo_fh->buf + demo_fh->size,
		    0, off - demo_fh->size);
	demo_fh->size = off;

	return (0);
}

/*
 * demo_file_write --
 *	POSIX pwrite.
 */
static int
demo_file_write(WT_FILE_HANDLE *file_handle, WT_SESSION *session,
    wt_off_t offset, size_t len, const void *buf)
{
	DEMO_FILE_HANDLE *demo_fh;
	int ret = 0;

	demo_fh = (DEMO_FILE_HANDLE *)file_handle;

	/* Make sure the buffer is large enough for the write */
	if ((ret = demo_file_truncate(file_handle, session,
	    offset + (wt_off_t)(len + DEMO_FILE_SIZE_INCREMENT))) != 0)
		return (ret);

	memcpy((uint8_t *)demo_fh->buf + offset, buf, len);
	demo_fh->off = (size_t)offset + len;

	return (0);
}

/*
 * demo_handle_remove --
 *	Destroy an in-memory file handle. Should only happen on remove or
 *	shutdown.
 */
static int
demo_handle_remove(WT_SESSION *session, DEMO_FILE_HANDLE *demo_fh)
{
	DEMO_FILE_SYSTEM *demo_fs;

	(void)session;						/* Unused */
	demo_fs = demo_fh->demo_fs;

	if (demo_fh->ref != 0) {
		fprintf(stderr,
		    "demo_handle_remove on file %s with non-zero reference "
		    "count of %u\n",
		    demo_fh->iface.name, demo_fh->ref);
		return (EINVAL);
	}

	TAILQ_REMOVE(&demo_fs->fileq, demo_fh, q);

	/* Clean up private information. */
	free(demo_fh->buf);
	demo_fh->buf = NULL;

	/* Clean up public information. */
	free(demo_fh->iface.name);

	free(demo_fh);

	return (0);
}

/*
 * demo_handle_search --
 *	Return a matching handle, if one exists.
 */
static DEMO_FILE_HANDLE *
demo_handle_search(WT_FILE_SYSTEM *file_system, const char *name)
{
	DEMO_FILE_HANDLE *demo_fh;
	DEMO_FILE_SYSTEM *demo_fs;

	demo_fs = (DEMO_FILE_SYSTEM *)file_system;

	TAILQ_FOREACH(demo_fh, &demo_fs->fileq, q)
		if (strcmp(demo_fh->iface.name, name) == 0)
			break;
	return (demo_fh);
}

int
main(void)
{
	WT_CONNECTION *conn;
	const char *open_config;
	int ret = 0;

	/*
	 * Create a clean test directory for this run of the test program if the
	 * environment variable isn't already set (as is done by make check).
	 */
	if (getenv("WIREDTIGER_HOME") == NULL) {
		home = "WT_HOME";
		ret = system("rm -rf WT_HOME && mkdir WT_HOME");
	} else
		home = NULL;

	/*! [WT_FILE_SYSTEM register] */
	/*
	 * Setup a configuration string that will load our custom file system.
	 * Use the special local extension to indicate that the entry point is
	 * in the same executable. Also enable early load for this extension,
	 * since WiredTiger needs to be able to find it before doing any file
	 * operations.
	 */
	open_config = "create,log=(enabled=true),extensions=(local="
	    "{entry=demo_file_system_create,early_load=true})";
	/* Open a connection to the database, creating it if necessary. */
	if ((ret = wiredtiger_open(home, NULL, open_config, &conn)) != 0) {
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
		return (ret);
	}
	/*! [WT_FILE_SYSTEM register] */

	if ((ret = conn->close(conn, NULL)) != 0)
		fprintf(stderr, "Error closing connection to %s: %s\n",
		    home, wiredtiger_strerror(ret));

	return (ret);
}
