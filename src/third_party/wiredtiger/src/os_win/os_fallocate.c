/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_fallocate_config --
 *	Configure fallocate behavior for a file handle.
 */
void
__wt_fallocate_config(WT_SESSION_IMPL *session, WT_FH *fh)
{
	WT_UNUSED(session);

	/*
	 * fallocate on Windows would be implemented using SetEndOfFile, which
	 * can also truncate the file. WiredTiger expects fallocate to ignore
	 * requests to truncate the file which Windows does not do, so we don't
	 * support the call.
	 */
	fh->fallocate_available = WT_FALLOCATE_NOT_AVAILABLE;
	fh->fallocate_requires_locking = false;
}

/*
 * __wt_fallocate --
 *	Allocate space for a file handle.
 */
int
__wt_fallocate(
    WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, wt_off_t len)
{
	WT_UNUSED(session);
	WT_UNUSED(fh);
	WT_UNUSED(offset);
	WT_UNUSED(len);

	return (ENOTSUP);
}
