/*-
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
	fh->fallocate_available = 1;

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
	WT_DECL_RET;
	LARGE_INTEGER largeint;

	WT_RET(__wt_verbose(
	    session, WT_VERB_FILEOPS, "%s: fallocate", fh->name));

	largeint.QuadPart = offset + len;

	if ((ret = SetFilePointerEx(
	    fh->filehandle_secondary, largeint, NULL, FILE_BEGIN)) == FALSE)
		WT_RET_MSG(session,
		    __wt_errno(), "%s SetFilePointerEx error", fh->name);

	if ((ret = SetEndOfFile(fh->filehandle_secondary)) != FALSE) {
		fh->size = fh->extend_size = len;
		return (0);
	}

	WT_RET_MSG(session, __wt_errno(), "%s SetEndOfFile error", fh->name);
}
