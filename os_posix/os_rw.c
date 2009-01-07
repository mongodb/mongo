/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_read --
 *	Read a file handle.
 */
int
__wt_read(ENV *env, WT_FH *fh, off_t offset, size_t bytes, void *buf)
{
	WT_STAT_INCR(fh, READ_IO, "count of read I/Os");

	if (FLD_ISSET(env->verbose, WT_VERB_FILEOPS_ALL))
		__wt_env_errx(env,
		    "fileops: %s: read %lu bytes at offset %lu",
		    fh->name, (u_long)bytes, (u_long)offset);

	if (pread(fh->fd, buf, bytes, offset) == (ssize_t)bytes)
		return (0);

	__wt_env_err(env, errno, "Read error; file %s", fh->name);
	return (WT_ERROR);
}

/*
 * __wt_write --
 *	Write a file handle.
 */
int
__wt_write(ENV *env, WT_FH *fh, off_t offset, size_t bytes, void *buf)
{
	WT_STAT_INCR(fh, WRITE_IO, "count of write I/Os");

	if (FLD_ISSET(env->verbose, WT_VERB_FILEOPS_ALL))
		__wt_env_errx(env,
		    "fileops: %s: write %lu bytes at offset %lu",
		    fh->name, (u_long)bytes, (u_long)offset);

	if (pwrite(fh->fd, buf, bytes, offset) == (ssize_t)bytes)
		return (0);

	__wt_env_err(env, errno, "Write error; file %s", fh->name);
	return (WT_ERROR);
}
