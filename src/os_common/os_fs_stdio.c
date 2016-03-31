/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __stdio_handle_advise --
 *	POSIX fadvise.
 */
static int
__stdio_handle_advise(WT_SESSION_IMPL *session,
    WT_FH *fh, wt_off_t offset, wt_off_t len, int advice)
{
	WT_UNUSED(offset);
	WT_UNUSED(len);
	WT_UNUSED(advice);
	WT_RET_MSG(session, ENOTSUP, "%s: handle-advise", fh->name);
}

/*
 * __stdio_handle_allocate --
 *	POSIX fallocate.
 */
static int
__stdio_handle_allocate(
    WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, wt_off_t len)
{
	WT_UNUSED(offset);
	WT_UNUSED(len);
	WT_RET_MSG(session, ENOTSUP, "%s: handle-allocate", fh->name);
}

/*
 * __stdio_handle_close --
 *	ANSI C close/fclose.
 */
static int
__stdio_handle_close(WT_SESSION_IMPL *session, WT_FH *fh)
{
	WT_RET_MSG(session, ENOTSUP, "%s: handle-close", fh->name);
}

/*
 * __stdio_handle_getc --
 *	ANSI C fgetc.
 */
static int
__stdio_handle_getc(WT_SESSION_IMPL *session, WT_FH *fh, int *chp)
{
	WT_UNUSED(chp);
	WT_RET_MSG(session, ENOTSUP, "%s: handle-getc", fh->name);
}

/*
 * __stdio_handle_lock --
 *	Lock/unlock a file.
 */
static int
__stdio_handle_lock(WT_SESSION_IMPL *session, WT_FH *fh, bool lock)
{
	WT_UNUSED(lock);
	WT_RET_MSG(session, ENOTSUP, "%s: handle-lock", fh->name);
}

/*
 * __stdio_handle_map --
 *	Map a file.
 */
static int
__stdio_handle_map(WT_SESSION_IMPL *session,
    WT_FH *fh, void *p, size_t *lenp, void **mappingcookie)
{
	WT_UNUSED(p);
	WT_UNUSED(lenp);
	WT_UNUSED(mappingcookie);
	WT_RET_MSG(session, ENOTSUP, "%s: handle-map", fh->name);
}

/*
 * __stdio_handle_map_discard --
 *	Discard a section of a mapped region.
 */
static int
__stdio_handle_map_discard(
    WT_SESSION_IMPL *session, WT_FH *fh, void *p, size_t len)
{
	WT_UNUSED(p);
	WT_UNUSED(len);
	WT_RET_MSG(session, ENOTSUP, "%s: handle-map-discard", fh->name);
}

/*
 * __stdio_handle_map_preload --
 *	Preload a section of a mapped region.
 */
static int
__stdio_handle_map_preload(
    WT_SESSION_IMPL *session, WT_FH *fh, const void *p, size_t len)
{
	WT_UNUSED(p);
	WT_UNUSED(len);
	WT_RET_MSG(session, ENOTSUP, "%s: handle-map-preload", fh->name);
}

/*
 * __stdio_handle_map_unmap --
 *	Unmap a file.
 */
static int
__stdio_handle_map_unmap(WT_SESSION_IMPL *session,
    WT_FH *fh, void *p, size_t len, void **mappingcookie)
{
	WT_UNUSED(p);
	WT_UNUSED(len);
	WT_UNUSED(mappingcookie);
	WT_RET_MSG(session, ENOTSUP, "%s: handle-map-unmap", fh->name);
}

/*
 * __stdio_handle_printf --
 *	ANSI C vfprintf.
 */
static int
__stdio_handle_printf(
    WT_SESSION_IMPL *session, WT_FH *fh, const char *fmt, va_list ap)
{
	if (vfprintf(fh->fp, fmt, ap) >= 0)
		return (0);
	WT_RET_MSG(session, EIO, "%s: handle-printf: vfprintf", fh->name);
}

/*
 * __stdio_handle_read --
 *	POSIX pread.
 */
static int
__stdio_handle_read(
    WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, size_t len, void *buf)
{
	WT_UNUSED(offset);
	WT_UNUSED(len);
	WT_UNUSED(buf);
	WT_RET_MSG(session, ENOTSUP, "%s: handle-read", fh->name);
}

/*
 * __stdio_handle_size --
 *	Get the size of a file in bytes, by file handle.
 */
static int
__stdio_handle_size(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t *sizep)
{
	WT_UNUSED(sizep);
	WT_RET_MSG(session, ENOTSUP, "%s: handle-size", fh->name);
}

/*
 * __stdio_handle_sync --
 *	POSIX fflush/fsync.
 */
static int
__stdio_handle_sync(WT_SESSION_IMPL *session, WT_FH *fh, bool block)
{
	WT_UNUSED(block);

	if (fflush(fh->fp) == 0)
		return (0);
	WT_RET_MSG(session, __wt_errno(), "%s: handle-sync: fflush", fh->name);
}

/*
 * __stdio_handle_truncate --
 *	POSIX ftruncate.
 */
static int
__stdio_handle_truncate(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t len)
{
	WT_UNUSED(len);
	WT_RET_MSG(session, ENOTSUP, "%s: handle-truncate", fh->name);
}

/*
 * __stdio_handle_write --
 *	POSIX pwrite.
 */
static int
__stdio_handle_write(WT_SESSION_IMPL *session,
    WT_FH *fh, wt_off_t offset, size_t len, const void *buf)
{
	WT_UNUSED(offset);
	WT_UNUSED(len);
	WT_UNUSED(buf);
	WT_RET_MSG(session, ENOTSUP, "%s: handle-write", fh->name);
}

/*
 * __stdio_func_init --
 *	Initialize stdio functions.
 */
static void
__stdio_func_init(WT_FH *fh, const char *name, FILE *fp)
{
	fh->name = name;
	fh->fp = fp;

	fh->fh_advise = __stdio_handle_advise;
	fh->fh_allocate = __stdio_handle_allocate;
	fh->fh_close = __stdio_handle_close;
	fh->fh_getc = __stdio_handle_getc;
	fh->fh_lock = __stdio_handle_lock;
	fh->fh_map = __stdio_handle_map;
	fh->fh_map_discard = __stdio_handle_map_discard;
	fh->fh_map_preload = __stdio_handle_map_preload;
	fh->fh_map_unmap = __stdio_handle_map_unmap;
	fh->fh_printf = __stdio_handle_printf;
	fh->fh_read = __stdio_handle_read;
	fh->fh_size = __stdio_handle_size;
	fh->fh_sync = __stdio_handle_sync;
	fh->fh_truncate = __stdio_handle_truncate;
	fh->fh_write = __stdio_handle_write;
}

/*
 * __wt_os_stdio --
 *	Initialize the stdio configuration.
 */
int
__wt_os_stdio(WT_SESSION_IMPL *session)
{
	__stdio_func_init(WT_STDERR(session), "stderr", stderr);
	__stdio_func_init(WT_STDOUT(session), "stdout", stdout);

	return (0);
}
