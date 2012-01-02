/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_fsync --
 *	Flush a file handle.
 */
int
__wt_fsync(WT_SESSION_IMPL *session, WT_FH *fh)
{
	int ret;

	WT_VERBOSE(session, fileops, "%s: fsync", fh->name);

	SYSCALL_RETRY(fsync(fh->fd), ret);
	if (ret == 0)
		return (0);

	WT_RET_MSG(session, ret, "%s fsync error", fh->name);
}
