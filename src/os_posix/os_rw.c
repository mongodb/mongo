/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_read --
 *	Read a chunk.
 */
int
__wt_read(SESSION *session, WT_FH *fh, off_t offset, uint32_t bytes, void *buf)
{
	WT_STAT_INCR(fh->stats, READ_IO);
	WT_STAT_INCR(S2C(session)->stats, TOTAL_READ_IO);

	WT_VERBOSE(S2C(session), WT_VERB_FILEOPS,
	    (session, "fileops: %s: read %lu bytes at offset %lu",
	    fh->name, (u_long)bytes, (u_long)offset));

	if (pread(fh->fd, buf, (size_t)bytes, offset) == (ssize_t)bytes)
		return (0);

	__wt_err(session, errno,
	    "%s read error: attempt to read %lu bytes at offset %lu",
	    fh->name, (u_long)bytes, (u_long)offset);
	return (WT_ERROR);
}

/*
 * __wt_write --
 *	Write a chunk.
 */
int
__wt_write(SESSION *session, WT_FH *fh, off_t offset, uint32_t bytes, void *buf)
{
	WT_STAT_INCR(fh->stats, WRITE_IO);
	WT_STAT_INCR(S2C(session)->stats, TOTAL_WRITE_IO);

	WT_VERBOSE(S2C(session), WT_VERB_FILEOPS,
	    (session, "fileops: %s: write %lu bytes at offset %lu",
	    fh->name, (u_long)bytes, (u_long)offset));

	if (pwrite(fh->fd, buf, (size_t)bytes, offset) == (ssize_t)bytes)
		return (0);

	__wt_err(session, errno,
	    "%s write error: attempt to write %lu bytes at offset %lu",
	    fh->name, (u_long)bytes, (u_long)offset);
	return (WT_ERROR);
}
