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
 * __wt_fsync --
 *	Flush a file handle.
 */
int
__wt_fsync(ENV *env, WT_FH *fh)
{

	WT_STAT_INCR(fh->stats, FSYNC);

	WT_VERBOSE(env, WT_VERB_FILEOPS, (env, "fileops: %s: fsync", fh->name));

	if (fsync(fh->fd) == 0)
		return (0);

	__wt_api_env_err(env, errno, "%s fsync error", fh->name);
	return (WT_ERROR);
}
