/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_ftruncate --
 *	Truncate a file.
 */
int
__wt_ftruncate(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t len)
{
	WT_DECL_RET;
	LARGE_INTEGER largeint;

	largeint.QuadPart = len;

	if ((ret = SetFilePointerEx(fh->filehandle, largeint, NULL, FILE_BEGIN)) == FALSE)
		WT_RET_MSG(session, __wt_errno(), "%s SetFilePointerEx error",
		    fh->name);

	if ((ret = SetEndOfFile(fh->filehandle)) != FALSE) {
		fh->size = fh->extend_size = len;
		return (0);
	}

	WT_RET_MSG(session, __wt_errno(), "%s SetEndofFile error", fh->name);
}
