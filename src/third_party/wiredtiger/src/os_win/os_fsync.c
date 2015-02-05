/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_directory_sync_fh --
 *	Flush a directory file handle.
 */
int
__wt_directory_sync_fh(WT_SESSION_IMPL *session, WT_FH *fh)
{
	WT_UNUSED(session);
	WT_UNUSED(fh);
	return (0);
}

/*
 * __wt_directory_sync --
 *	Flush a directory to ensure a file creation is durable.
 */
int
__wt_directory_sync(WT_SESSION_IMPL *session, char *path)
{
	WT_UNUSED(session);
	WT_UNUSED(path);
	return (0);
}

/*
 * __wt_fsync --
 *	Flush a file handle.
 */
int
__wt_fsync(WT_SESSION_IMPL *session, WT_FH *fh)
{
	WT_DECL_RET;

	WT_RET(__wt_verbose(session, WT_VERB_FILEOPS, "%s: FlushFileBuffers",
	    fh->name));

	if ((ret = FlushFileBuffers(fh->filehandle)) == FALSE)
		WT_RET_MSG(session,
		    __wt_errno(), "%s FlushFileBuffers error", fh->name);

	return (0);
}

/*
 * __wt_fsync_async --
 *	Flush a file handle and don't wait for the result.
 */
int
__wt_fsync_async(WT_SESSION_IMPL *session, WT_FH *fh)
{
	WT_UNUSED(session);
	WT_UNUSED(fh);

	return (0);
}
