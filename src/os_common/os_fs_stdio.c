/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

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
 * __stdio_func_init --
 *	Initialize stdio functions.
 */
static void
__stdio_func_init(WT_FH *fh, const char *name, FILE *fp)
{
	fh->name = name;
	fh->fp = fp;

	fh->fh_printf = __stdio_handle_printf;
	fh->fh_sync = __stdio_handle_sync;
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
