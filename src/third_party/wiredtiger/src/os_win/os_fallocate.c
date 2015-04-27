/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
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
	 * fallocate on Windows is implemented using SetEndOfFile which can
	 * also truncate the file. WiredTiger expects fallocate to ignore
	 * requests to truncate the file which Windows does not do.
	 */
	fh->fallocate_available = WT_FALLOCATE_NOT_AVAILABLE;

	/*
	 * We use a separate handle for file size changes, so there's no need
	 * for locking.
	 */
	fh->fallocate_requires_locking = 0;
}

/*
 * __wt_fallocate --
 *	Allocate space for a file handle.
 */
int
__wt_fallocate(
    WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, wt_off_t len)
{
	return (ENOTSUP);
}
