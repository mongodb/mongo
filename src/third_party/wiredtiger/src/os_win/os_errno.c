/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_errno --
 *	Return errno, or WT_ERROR if errno not set.
 */
int
__wt_errno(void)
{
	/*
	 * Called when we know an error occurred, and we want the system
	 * error code, but there's some chance it's not set.
	 */
	DWORD err = GetLastError();

	/* GetLastError should only be called if we hit an actual error */
	WT_ASSERT(NULL, err != ERROR_SUCCESS);

	return (err == ERROR_SUCCESS ? WT_ERROR : err);
}
