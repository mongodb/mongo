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
__wt_fsync(SESSION *session, WT_FH *fh)
{

	WT_STAT_INCR(fh->stats, FSYNC);

	WT_VERBOSE(S2C(session), WT_VERB_FILEOPS,
	    (session, "fileops: %s: fsync", fh->name));

	if (fsync(fh->fd) == 0)
		return (0);

	__wt_err(session, errno, "%s fsync error", fh->name);
	return (WT_ERROR);
}
