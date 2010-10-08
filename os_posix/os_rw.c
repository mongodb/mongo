/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_read --
 *	Read a chunk.
 */
int
__wt_read(ENV *env, WT_FH *fh, off_t offset, u_int32_t bytes, void *buf)
{
	WT_STAT_INCR(fh->stats, READ_IO);
	WT_STAT_INCR(env->ienv->stats, TOTAL_READ_IO);

	WT_VERBOSE(env, WT_VERB_FILEOPS,
	    (env, "fileops: %s: read %lu bytes at offset %lu",
	    fh->name, (u_long)bytes, (u_long)offset));

	if (pread(fh->fd, buf, (size_t)bytes, offset) == (ssize_t)bytes)
		return (0);

	__wt_api_env_err(env, errno,
	    "%s read error: attempt to read %lu bytes at offset %lu",
	    fh->name, (u_long)bytes, (u_long)offset);
	return (WT_ERROR);
}

/*
 * __wt_write --
 *	Write a chunk.
 */
int
__wt_write(ENV *env, WT_FH *fh, off_t offset, u_int32_t bytes, void *buf)
{
	WT_STAT_INCR(fh->stats, WRITE_IO);
	WT_STAT_INCR(env->ienv->stats, TOTAL_WRITE_IO);

	WT_VERBOSE(env, WT_VERB_FILEOPS,
	    (env, "fileops: %s: write %lu bytes at offset %lu",
	    fh->name, (u_long)bytes, (u_long)offset));

	if (pwrite(fh->fd, buf, (size_t)bytes, offset) == (ssize_t)bytes)
		return (0);

	__wt_api_env_err(env, errno,
	    "%s write error: attempt to write %lu bytes at offset %lu",
	    fh->name, (u_long)bytes, (u_long)offset);
	return (WT_ERROR);
}
