/*-
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

/*
 * __wt_strerror_r --
 *	Windows implementation of strerror_r.
 */
int
__wt_strerror_r(int error, char *buf, size_t buflen)
{
	char *p;

	/* Require at least 2 bytes, printable character and trailing nul. */
	if (buflen < 2)
		return (ENOMEM);

	/*
	 * POSIX errors are non-negative integers, copy the string into the
	 * user's buffer. Return success if anything printed (we checked if
	 * the buffer had space for at least one character).
	 */
	if (error > 0 &&
	    (p = strerror(error)) != NULL && snprintf(buf, buflen, "%s", p) > 0)
		return (0);

	/* Fallback to a generic message, then guess it's a memory problem. */
	return (
	    snprintf(buf, buflen, "error return: %d", error) > 0 ? 0 : ENOMEM);
}
