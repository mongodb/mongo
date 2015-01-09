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
	return (errno == 0 ? WT_ERROR : errno);
}

/*
 * __wt_strerror --
 *	POSIX implementation of wiredtiger_strerror.
 */
const char *
__wt_strerror(int error)
{
	const char *p;

	/*
	 * POSIX errors are non-negative integers; check for 0 explicitly
	 * in-case the underlying strerror doesn't handle 0, some don't.
	 */
	if (error == 0)
		return ("Successful return: 0");
	if (error > 0 && (p = strerror(error)) != NULL)
		return (p);
	return (NULL);
}

/*
 * __wt_strerror_r --
 *	POSIX implementation of wiredtiger_strerror_r.
 */
int
__wt_strerror_r(int error, char *buf, size_t buflen)
{
	const char *p;

	/* Require at least 2 bytes, printable character and trailing nul. */
	if (buflen < 2)
		return (ENOMEM);

	/*
	 * Check for POSIX errors then fallback to something generic.  Copy the
	 * string into the user's buffer, return success if anything printed.
	 */
	p = __wt_strerror(error);
	if (p != NULL && snprintf(buf, buflen, "%s", p) > 0)
		return (0);

	/* Fallback to a generic message, then guess it's a memory problem. */
	return (
	    snprintf(buf, buflen, "error return: %d", error) > 0 ? 0 : ENOMEM);
}
