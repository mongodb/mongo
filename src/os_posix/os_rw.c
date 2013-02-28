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

	if (pread(fh->fd, buf, (size_t)bytes, offset) != (ssize_t)bytes)
		WT_RET_MSG(session, __wt_errno(),
		    "%s read error: failed to read %" PRIu32
		    " bytes at offset %" PRIuMAX,
		    fh->name, bytes, (uintmax_t)offset);

#if HAVE_POSIX_FADVISE
	if (fh->os_cache_max > 0 &&
	    (fh->io_size += (off_t)bytes) > fh->os_cache_max) {
		fh->io_size = 0;
		WT_RET(posix_fadvise(fh->fd, 0, 0, POSIX_FADV_DONTNEED));
	}
#endif

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

	if (pwrite(fh->fd, buf, (size_t)bytes, offset) != (ssize_t)bytes)
		WT_RET_MSG(session, __wt_errno(),
		    "%s write error: failed to write %" PRIu32
		    " bytes at offset %" PRIuMAX,
		    fh->name, bytes, (uintmax_t)offset);

#if HAVE_POSIX_FADVISE
	if (fh->os_cache_max > 0)
		fh->io_size += (off_t)bytes;
#endif
	return (0);
}
