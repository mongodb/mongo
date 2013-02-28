/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_fsync --
 *	Flush a file handle.
 */
int
__wt_fsync(WT_SESSION_IMPL *session, WT_FH *fh)
{
	WT_DECL_RET;

	WT_VERBOSE_RET(session, fileops, "%s: fsync", fh->name);

	WT_SYSCALL_RETRY(fsync(fh->fd), ret);
	if (ret != 0)
		WT_RET_MSG(session, ret, "%s fsync error", fh->name);

#if HAVE_POSIX_FADVISE
	if (fh->os_cache_max > 0 && fh->io_size > fh->os_cache_max) {
		fh->io_size = 0;
		WT_RET(posix_fadvise(fh->fd, 0, 0, POSIX_FADV_DONTNEED));
	}
#endif

	return (0);
}
