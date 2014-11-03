/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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

	WT_RET(__wt_verbose(session, WT_VERB_FILEOPS, "%s: fsync", fh->name));

#ifdef HAVE_FDATASYNC
	WT_SYSCALL_RETRY(fdatasync(fh->fd), ret);
#else
	WT_SYSCALL_RETRY(fsync(fh->fd), ret);
#endif
	if (ret != 0)
		WT_RET_MSG(session, ret, "%s fsync error", fh->name);

	return (0);
}

/*
 * __wt_fsync_async --
 *	Flush a file handle and don't wait for the result.
 */
int
__wt_fsync_async(WT_SESSION_IMPL *session, WT_FH *fh)
{
#ifdef	HAVE_SYNC_FILE_RANGE
	WT_DECL_RET;

	WT_RET(__wt_verbose(
	    session, WT_VERB_FILEOPS, "%s: sync_file_range", fh->name));

	if ((ret = sync_file_range(fh->fd,
	    (off64_t)0, (off64_t)0, SYNC_FILE_RANGE_WRITE)) == 0)
		return (0);
	WT_RET_MSG(session, ret, "%s: sync_file_range", fh->name);
#else
	WT_UNUSED(session);
	WT_UNUSED(fh);
	return (0);
#endif
}
