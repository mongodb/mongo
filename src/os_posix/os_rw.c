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
__wt_read(WT_SESSION_IMPL *session,
    WT_FH *fh, off_t offset, uint32_t bytes, void *buf)
{
	WT_STAT_INCR(fh->stats, read_io);
	WT_STAT_INCR(S2C(session)->stats, total_read_io);

	WT_VERBOSE(S2C(session), WT_VERB_FILEOPS, (session,
	    "fileops: %s: read %" PRIu32 " bytes at offset %" PRIuMAX,
	    fh->name, bytes, (uintmax_t)offset));

	if (pread(fh->fd, buf, (size_t)bytes, offset) == (ssize_t)bytes)
		return (0);

	__wt_err(session, errno, "%s read error: attempt to read %" PRIu32
	    " bytes at offset %" PRIuMAX,
	    fh->name, bytes, (uintmax_t)offset);
	return (WT_ERROR);
}

/*
 * __wt_write --
 *	Write a chunk.
 */
int
__wt_write(WT_SESSION_IMPL *session,
    WT_FH *fh, off_t offset, uint32_t bytes, void *buf)
{
	WT_STAT_INCR(fh->stats, write_io);
	WT_STAT_INCR(S2C(session)->stats, total_write_io);

	WT_VERBOSE(S2C(session), WT_VERB_FILEOPS, (session,
	    "fileops: %s: write %" PRIu32 " bytes at offset %" PRIuMAX,
	    fh->name, bytes, (uintmax_t)offset));

	if (pwrite(fh->fd, buf, (size_t)bytes, offset) == (ssize_t)bytes)
		return (0);

	__wt_err(session, errno, "%s write error: attempt to write %" PRIu32
	    " bytes at offset %" PRIuMAX,
	    fh->name, bytes, (uintmax_t)offset);
	return (WT_ERROR);
}
