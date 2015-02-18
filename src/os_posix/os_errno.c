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
 * __wt_session_strerror --
 *	POSIX implementation of WT_SESSION.strerror.
 */
const char *
__wt_session_strerror(WT_SESSION_IMPL *session, int error)
{
	const char *p;

	/* Check for POSIX errors. */
	if ((p = __wt_strerror(error)) != NULL)
		return (p);

	/* Fallback to a generic message. */
	if (__wt_buf_fmt(
	    session, &session->err, "error return: %d", error) == 0)
		return (session->err.data);

	/* Defeated. */
	return ("Unable to return error string");
}
