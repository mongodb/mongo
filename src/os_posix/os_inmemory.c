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
 * __im_file_exist --
 *	Return if the file exists.
 */
static int
__im_file_exist(WT_SESSION_IMPL *session, const char *name, bool *existp)
{
	*existp = __wt_handle_search(session, name, false, true, NULL, NULL);
	return (0);
}

/*
 * __im_file_remove --
 *	POSIX remove.
 */
static int
__im_file_remove(WT_SESSION_IMPL *session, const char *name)
{
	WT_DECL_RET;
	WT_FH *fh;

	if (__wt_handle_search(session, name, true, true, NULL, &fh)) {
		WT_ASSERT(session, fh->ref == 1);

		/* Force a discard of the handle. */
		F_CLR(fh, WT_FH_IN_MEMORY);
		ret = __wt_close(session, &fh);
	}
	return (ret);
}

/*
 * __im_file_rename --
 *	POSIX rename.
 */
static int
__im_file_rename(WT_SESSION_IMPL *session, const char *from, const char *to)
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
 * __im_file_size --
 *	Get the size of a file in bytes, by file name.
 */
static int
__im_file_size(
    WT_SESSION_IMPL *session, const char *name, bool silent, wt_off_t *sizep)
{
	WT_DECL_RET;
	WT_FH *fh;
	WT_IM *im;

	WT_UNUSED(silent);

	im = __wt_process.inmemory;
	__wt_spin_lock(session, &im->lock);

	if (__wt_handle_search(session, name, false, false, NULL, &fh)) {
		*sizep = (wt_off_t)fh->buf.size;
		__wt_handle_search_unlock(session);
	} else
		ret = ENOENT;

	__wt_spin_unlock(session, &im->lock);
	return (ret);
}

/*
 * __im_handle_advise --
 *	POSIX fadvise.
 */
static int
__im_handle_advise(WT_SESSION_IMPL *session,
    WT_FH *fh, wt_off_t offset, wt_off_t len, int advice)
{
	WT_UNUSED(session);
	WT_UNUSED(fh);
	WT_UNUSED(offset);
	WT_UNUSED(len);
	WT_UNUSED(advice);
	return (0);
}

/*
 * __im_handle_close --
 *	ANSI C close/fclose.
 */
static int
__im_handle_close(WT_SESSION_IMPL *session, WT_FH *fh)
{
	__wt_buf_free(session, &fh->buf);

	return (0);
}

/*
 * __im_handle_getc --
 *	ANSI C fgetc.
 */
static int
__im_handle_getc(WT_SESSION_IMPL *session, WT_FH *fh, int *chp)
{
	WT_IM *im;

	im = __wt_process.inmemory;
	__wt_spin_lock(session, &im->lock);

	if (fh->off >= fh->buf.size)
		*chp = EOF;
	else
		*chp = ((char *)fh->buf.data)[fh->off++];

	__wt_spin_unlock(session, &im->lock);
	return (0);
}

/*
 * __im_handle_lock --
 *	Lock/unlock a file.
 */
static int
__im_handle_lock(WT_SESSION_IMPL *session, WT_FH *fh, bool lock)
{
	WT_UNUSED(session);
	WT_UNUSED(fh);
	WT_UNUSED(lock);
	return (0);
}

/*
 * __im_handle_open --
 *	POSIX fopen/open.
 */
static int
__im_handle_open(WT_SESSION_IMPL *session,
    WT_FH *fh, const char *path, int dio_type, u_int flags)
{
	WT_UNUSED(session);
	WT_UNUSED(path);
	WT_UNUSED(dio_type);
	WT_UNUSED(flags);

	fh->off = 0;
	F_SET(fh, WT_FH_IN_MEMORY);

	return (0);
}

/*
 * __im_handle_printf --
 *	ANSI C vfprintf.
 */
static int
__im_handle_printf(
    WT_SESSION_IMPL *session, WT_FH *fh, const char *fmt, va_list ap)
{
	va_list ap_copy;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_IM *im;
	size_t len;

	im = __wt_process.inmemory;

	if (fh == WT_STDERR || fh == WT_STDOUT) {
		if (vfprintf(fh == WT_STDERR ? stderr : stdout, fmt, ap) >= 0)
			return (0);
		WT_RET_MSG(session, EIO,
		    "%s: vfprintf", fh == WT_STDERR ? "stderr" : "stdout");
	}

	/* Build the string we're writing. */
	WT_RET(__wt_scr_alloc(session, strlen(fmt) * 2 + 128, &tmp));
	for (;;) {
		va_copy(ap_copy, ap);
		len = (size_t)vsnprintf(tmp->mem, tmp->memsize, fmt, ap_copy);
		if (len < tmp->memsize) {
			tmp->data = tmp->mem;
			tmp->size = len;
			break;
		}
		WT_ERR(__wt_buf_extend(session, tmp, len + 1));
	}

	__wt_spin_lock(session, &im->lock);

	/* Grow the handle's buffer as necessary. */
	WT_ERR(__wt_buf_grow(session, &fh->buf, fh->off + len));

	/* Copy the data into place and update the offset. */
	memcpy((uint8_t *)fh->buf.mem + fh->off, tmp->data, len);
	fh->off += len;

err:	__wt_spin_unlock(session, &im->lock);

	__wt_scr_free(session, &tmp);
	return (ret);
}

/*
 * __im_handle_read --
 *	POSIX pread.
 */
static int
__im_handle_read(
    WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, size_t len, void *buf)
{
	WT_DECL_RET;
	WT_IM *im;
	size_t off;

	im = __wt_process.inmemory;
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
	    "%s read error: failed to read %" WT_SIZET_FMT " bytes at "
	    "offset %" WT_SIZET_FMT,
	    fh->name, len, off);
}

/*
 * __im_handle_size --
 *	Get the size of a file in bytes, by file handle.
 */
static int
__im_handle_size(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t *sizep)
{
	WT_UNUSED(session);

	*sizep = (wt_off_t)fh->buf.size;
	return (0);
}

/*
 * __im_handle_sync --
 *	POSIX fflush/fsync.
 */
static int
__im_handle_sync(WT_SESSION_IMPL *session, WT_FH *fh, bool block)
{
	WT_UNUSED(block);

	/* Flush any stream's stdio buffers. */
	if (fh == WT_STDERR || fh == WT_STDOUT) {
		if (fflush(fh == WT_STDERR ? stderr : stdout) == 0)
			return (0);
		WT_RET_MSG(session, __wt_errno(),
		    "%s: fflush", fh == WT_STDERR ? "stderr" : "stdout");
	}
	return (0);
}

/*
 * __im_handle_truncate --
 *	POSIX ftruncate.
 */
static int
__im_handle_truncate(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t len)
{
	WT_DECL_RET;
	WT_IM *im;

	im = __wt_process.inmemory;
	__wt_spin_lock(session, &im->lock);

	WT_ERR(__wt_buf_grow(session, &fh->buf, (size_t)len));
	memset((uint8_t *)
	    fh->buf.mem + fh->buf.size, 0, fh->buf.memsize - fh->buf.size);

err:	__wt_spin_unlock(session, &im->lock);
	return (ret);
}

/*
 * __im_handle_write --
 *	POSIX pwrite.
 */
static int
__im_handle_write(WT_SESSION_IMPL *session,
    WT_FH *fh, wt_off_t offset, size_t len, const void *buf)
{
	WT_DECL_RET;
	WT_IM *im;
	size_t off;

	im = __wt_process.inmemory;
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
	    "%s write error: failed to write %" WT_SIZET_FMT " bytes at "
	    "offset %" WT_SIZET_FMT,
	    fh->name, len, off);
}

/*
 * __wt_os_inmemory --
 *	Initialize an in-memory configuration.
 */
int
__wt_os_inmemory(WT_SESSION_IMPL *session)
{
	WT_DECL_RET;
	WT_IM *im;

	im = NULL;

	/* Allocate an in-memory structure. */
	WT_RET(__wt_calloc_one(session, &im));
	WT_ERR(__wt_spin_init(session, &im->lock, "in-memory I/O"));

	/* Initialize the in-memory jump table. */
	__wt_process.j_directory_sync = __im_directory_sync;
	__wt_process.j_file_exist = __im_file_exist;
	__wt_process.j_file_remove = __im_file_remove;
	__wt_process.j_file_rename = __im_file_rename;
	__wt_process.j_file_size = __im_file_size;
	__wt_process.j_handle_advise = __im_handle_advise;
	__wt_process.j_handle_close = __im_handle_close;
	__wt_process.j_handle_getc = __im_handle_getc;
	__wt_process.j_handle_lock = __im_handle_lock;
	__wt_process.j_handle_open = __im_handle_open;
	__wt_process.j_handle_printf = __im_handle_printf;
	__wt_process.j_handle_read = __im_handle_read;
	__wt_process.j_handle_size = __im_handle_size;
	__wt_process.j_handle_sync = __im_handle_sync;
	__wt_process.j_handle_truncate = __im_handle_truncate;
	__wt_process.j_handle_write = __im_handle_write;

	__wt_process.inmemory = im;
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

	if ((im = __wt_process.inmemory) == NULL)
		return (0);
	__wt_process.inmemory = NULL;

	__wt_spin_destroy(session, &im->lock);

	__wt_free(session, im);

	return (ret);
}
