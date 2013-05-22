/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
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
	WT_CSTAT_INCR(session, read_io);

	WT_VERBOSE_RET(session, fileops,
	    "%s: read %" PRIu32 " bytes at offset %" PRIuMAX,
	    fh->name, bytes, (uintmax_t)offset);

	WT_ASSERT(session, 		/* Assert aligned I/O is aligned. */
	    !fh->direct_io ||
	    S2C(session)->buffer_alignment == 0 ||
	    !((uintptr_t)buf &
	    (uintptr_t)(S2C(session)->buffer_alignment - 1)));

	if (pread(fh->fd, buf, (size_t)bytes, offset) != (ssize_t)bytes)
		WT_RET_MSG(session, __wt_errno(),
		    "%s read error: failed to read %" PRIu32
		    " bytes at offset %" PRIuMAX,
		    fh->name, bytes, (uintmax_t)offset);

	return (0);
}

/*
 * __wt_write --
 *	Write a chunk.
 */
int
__wt_write(WT_SESSION_IMPL *session,
    WT_FH *fh, off_t offset, uint32_t bytes, const void *buf)
{
	WT_CSTAT_INCR(session, write_io);

	WT_VERBOSE_RET(session, fileops,
	    "%s: write %" PRIu32 " bytes at offset %" PRIuMAX,
	    fh->name, bytes, (uintmax_t)offset);

	WT_ASSERT(session, 		/* Assert aligned I/O is aligned. */
	    !fh->direct_io ||
	    S2C(session)->buffer_alignment == 0 ||
	    !((uintptr_t)buf &
	    (uintptr_t)(S2C(session)->buffer_alignment - 1)));

	if (pwrite(fh->fd, buf, (size_t)bytes, offset) != (ssize_t)bytes)
		WT_RET_MSG(session, __wt_errno(),
		    "%s write error: failed to write %" PRIu32
		    " bytes at offset %" PRIuMAX,
		    fh->name, bytes, (uintmax_t)offset);

	return (0);
}
