/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * In-memory information.
 */
typedef struct {
	WT_SPINLOCK lock;
} WT_IM;

/*
 * __im_directory_list --
 *	Get a list of files from a directory, in-memory version.
 */
static int
__im_directory_list(WT_SESSION_IMPL *session, const char *dir,
    const char *prefix, uint32_t flags, char ***dirlist, u_int *countp)
{
	WT_UNUSED(session);
	WT_UNUSED(dir);
	WT_UNUSED(prefix);
	WT_UNUSED(flags);
	WT_UNUSED(dirlist);
	WT_UNUSED(countp);

	WT_RET_MSG(session, ENOTSUP, "directory-list");
}

/*
 * __im_directory_sync --
 *	Flush a directory to ensure file creation is durable.
 */
static int
__im_directory_sync(WT_SESSION_IMPL *session, const char *path)
{
	WT_UNUSED(session);
	WT_UNUSED(path);
	return (0);
}

/*
 * __im_fs_exist --
 *	Return if the file exists.
 */
static int
__im_fs_exist(WT_SESSION_IMPL *session, const char *name, bool *existp)
{
	*existp = __wt_handle_search(session, name, false, NULL, NULL);
	return (0);
}

/*
 * __im_fs_remove --
 *	POSIX remove.
 */
static int
__im_fs_remove(WT_SESSION_IMPL *session, const char *name)
{
	WT_DECL_RET;
	WT_FH *fh;

	if (__wt_handle_search(session, name, true, NULL, &fh)) {
		WT_ASSERT(session, fh->ref == 1);

		/* Force a discard of the handle. */
		F_CLR(fh, WT_FH_IN_MEMORY);
		ret = __wt_close(session, &fh);
	}
	return (ret);
}

/*
 * __im_fs_rename --
 *	POSIX rename.
 */
static int
__im_fs_rename(WT_SESSION_IMPL *session, const char *from, const char *to)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_FH *fh;
	uint64_t bucket, hash;
	char *to_name;

	conn = S2C(session);

	/* We'll need a copy of the target name. */
	WT_RET(__wt_strdup(session, to, &to_name));

	__wt_spin_lock(session, &conn->fh_lock);

	/* Make sure the target name isn't active. */
	hash = __wt_hash_city64(to, strlen(to));
	bucket = hash % WT_HASH_ARRAY_SIZE;
	TAILQ_FOREACH(fh, &conn->fhhash[bucket], hashq)
		if (strcmp(to, fh->name) == 0)
			WT_ERR(EPERM);

	/* Find the source name. */
	hash = __wt_hash_city64(from, strlen(from));
	bucket = hash % WT_HASH_ARRAY_SIZE;
	TAILQ_FOREACH(fh, &conn->fhhash[bucket], hashq)
		if (strcmp(from, fh->name) == 0)
			break;
	if (fh == NULL)
		WT_ERR(ENOENT);

	/* Remove source from the list. */
	WT_CONN_FILE_REMOVE(conn, fh, bucket);

	/* Swap the names. */
	__wt_free(session, fh->name);
	fh->name = to_name;
	to_name = NULL;

	/* Put source back on the list. */
	hash = __wt_hash_city64(to, strlen(to));
	bucket = hash % WT_HASH_ARRAY_SIZE;
	WT_CONN_FILE_INSERT(conn, fh, bucket);

	if (0) {
err:		__wt_free(session, to_name);
	}
	__wt_spin_unlock(session, &conn->fh_lock);

	return (ret);
}

/*
 * __im_fs_size --
 *	Get the size of a file in bytes, by file name.
 */
static int
__im_fs_size(
    WT_SESSION_IMPL *session, const char *name, bool silent, wt_off_t *sizep)
{
	WT_DECL_RET;
	WT_FH *fh;
	WT_IM *im;

	WT_UNUSED(silent);

	im = S2C(session)->inmemory;
	__wt_spin_lock(session, &im->lock);

	if (__wt_handle_search(session, name, true, NULL, &fh)) {
		WT_ERR(fh->fh_size(session, fh, sizep));
		WT_ERR(__wt_close(session, &fh));
	} else
		ret = ENOENT;

err:	__wt_spin_unlock(session, &im->lock);
	return (ret);
}

/*
 * __im_file_close --
 *	ANSI C close.
 */
static int
__im_file_close(WT_SESSION_IMPL *session, WT_FH *fh)
{
	__wt_buf_free(session, &fh->buf);

	return (0);
}

/*
 * __im_file_lock --
 *	Lock/unlock a file.
 */
static int
__im_file_lock(WT_SESSION_IMPL *session, WT_FH *fh, bool lock)
{
	/* Locks are always granted. */
	WT_UNUSED(session);
	WT_UNUSED(fh);
	WT_UNUSED(lock);
	return (0);
}

/*
 * __im_file_read --
 *	POSIX pread.
 */
static int
__im_file_read(
    WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, size_t len, void *buf)
{
	WT_DECL_RET;
	WT_IM *im;
	size_t off;

	im = S2C(session)->inmemory;
	__wt_spin_lock(session, &im->lock);

	off = (size_t)offset;
	if (off < fh->buf.size) {
		len = WT_MIN(len, fh->buf.size - off);
		memcpy(buf, (uint8_t *)fh->buf.mem + off, len);
		fh->off = off + len;
	} else
		ret = WT_ERROR;

	__wt_spin_unlock(session, &im->lock);
	if (ret == 0)
		return (0);
	WT_RET_MSG(session, WT_ERROR,
	    "%s: handle-read: failed to read %" WT_SIZET_FMT " bytes at "
	    "offset %" WT_SIZET_FMT,
	    fh->name, len, off);
}

/*
 * __im_file_size --
 *	Get the size of a file in bytes, by file handle.
 */
static int
__im_file_size(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t *sizep)
{
	WT_UNUSED(session);

	/*
	 * XXX hack - MongoDB assumes that any file with content will have a
	 * non-zero size. In memory tables generally are zero-sized, make
	 * MongoDB happy.
	 */
	*sizep = fh->buf.size == 0 ? 1024 : (wt_off_t)fh->buf.size;
	return (0);
}

/*
 * __im_file_sync --
 *	POSIX fflush/fsync.
 */
static int
__im_file_sync(WT_SESSION_IMPL *session, WT_FH *fh, bool block)
{
	WT_UNUSED(session);
	WT_UNUSED(fh);

	/*
	 * Callers attempting asynchronous flush handle ENOTSUP returns, and
	 * won't make further attempts.
	 */
	return (block ? 0 : ENOTSUP);
}

/*
 * __im_file_truncate --
 *	POSIX ftruncate.
 */
static int
__im_file_truncate(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset)
{
	WT_DECL_RET;
	WT_IM *im;
	size_t off;

	im = S2C(session)->inmemory;
	__wt_spin_lock(session, &im->lock);

	/*
	 * Grow the buffer as necessary, clear any new space in the file,
	 * and reset the file's data length.
	 */
	off = (size_t)offset;
	WT_ERR(__wt_buf_grow(session, &fh->buf, off));
	if (fh->buf.size < off)
		memset((uint8_t *)
		    fh->buf.data + fh->buf.size, 0, off - fh->buf.size);
	fh->buf.size = off;

err:	__wt_spin_unlock(session, &im->lock);
	return (ret);
}

/*
 * __im_file_write --
 *	POSIX pwrite.
 */
static int
__im_file_write(WT_SESSION_IMPL *session,
    WT_FH *fh, wt_off_t offset, size_t len, const void *buf)
{
	WT_DECL_RET;
	WT_IM *im;
	size_t off;

	im = S2C(session)->inmemory;
	__wt_spin_lock(session, &im->lock);

	off = (size_t)offset;
	WT_ERR(__wt_buf_grow(session, &fh->buf, off + len + 1024));

	memcpy((uint8_t *)fh->buf.data + off, buf, len);
	if (off + len > fh->buf.size)
		fh->buf.size = off + len;
	fh->off = off + len;

err:	__wt_spin_unlock(session, &im->lock);
	if (ret == 0)
		return (0);
	WT_RET_MSG(session, ret,
	    "%s: handle-write: failed to write %" WT_SIZET_FMT " bytes at "
	    "offset %" WT_SIZET_FMT,
	    fh->name, len, off);
}

/*
 * __im_file_open --
 *	POSIX fopen/open.
 */
static int
__im_file_open(WT_SESSION_IMPL *session,
    WT_FH *fh, const char *path, uint32_t file_type, uint32_t flags)
{
	WT_UNUSED(session);
	WT_UNUSED(path);
	WT_UNUSED(file_type);
	WT_UNUSED(flags);

	/*
	 * Unlike other file handle open implementations, the in-memory version
	 * is called whenever the WT_FH structure reference count goes to 0.
	 * This is because the in-memory implementation reuses WT_FH structures,
	 * and so we have to reset the file offset and potentially the list of
	 * functions, in the case of the file being opened in a different way.
	 */
	fh->off = 0;
	F_SET(fh, WT_FH_IN_MEMORY);

	fh->fh_close = __im_file_close;
	fh->fh_lock = __im_file_lock;
	fh->fh_read = __im_file_read;
	fh->fh_size = __im_file_size;
	fh->fh_sync = __im_file_sync;
	fh->fh_truncate = __im_file_truncate;
	fh->fh_write = __im_file_write;

	return (0);
}

/*
 * __wt_os_inmemory --
 *	Initialize an in-memory configuration.
 */
int
__wt_os_inmemory(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_IM *im;

	conn = S2C(session);
	im = NULL;

	/* Initialize the in-memory jump table. */
	conn->file_directory_list = __im_directory_list;
	conn->file_directory_sync = __im_directory_sync;
	conn->file_exist = __im_fs_exist;
	conn->file_remove = __im_fs_remove;
	conn->file_rename = __im_fs_rename;
	conn->file_size = __im_fs_size;
	conn->file_open = __im_file_open;

	/* Allocate an in-memory structure. */
	WT_RET(__wt_calloc_one(session, &im));
	WT_ERR(__wt_spin_init(session, &im->lock, "in-memory I/O"));
	conn->inmemory = im;

	return (0);

err:	__wt_free(session, im);
	return (ret);
}

/*
 * __wt_os_inmemory_cleanup --
 *	Discard an in-memory configuration.
 */
int
__wt_os_inmemory_cleanup(WT_SESSION_IMPL *session)
{
	WT_DECL_RET;
	WT_IM *im;

	if ((im = S2C(session)->inmemory) == NULL)
		return (0);
	S2C(session)->inmemory = NULL;

	__wt_spin_destroy(session, &im->lock);
	__wt_free(session, im);

	return (ret);
}
