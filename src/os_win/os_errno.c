/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static const int windows_error_offset = -29000;

/*
 * __wt_map_error_to_windows_error --
 *	Return a negative integer, an encoded Windows error
 * Standard C errors are positive integers from 0 - ~200
 * Windows errors are from 0 - 15999 according to the documentation
 */
static DWORD
__wt_map_error_to_windows_error(int error) {
	/* Ensure we do not exceed the error range
	   Also validate he do not get any COM errors
	   (which are negative integers)
	*/
	WT_ASSERT(NULL, error > 0 && error > -(windows_error_offset));

	return (error + -(windows_error_offset));
}

/*
 * __wt_map_error_to_windows_error --
 *	Return a positive integer, a decoded Windows error
 */
static int
__wt_map_windows_error_to_error(DWORD winerr) {
	return (winerr + windows_error_offset);
}

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

	return (err == ERROR_SUCCESS ?
	    WT_ERROR : __wt_map_windows_error_to_error(err));
}

/*
 * __wt_strerror --
 *	Windows implementation of wiredtiger_strerror.
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
 *	Windows implementation of WT_SESSION.strerror.
 */
const char *
__wt_session_strerror(WT_SESSION_IMPL *session, int error)
{
	DWORD lasterror;
	const char *p;
	char buf[256];

	/*
	 * Check for POSIX errors, Windows errors, then fallback to something
	 * generic.  Copy the string into the user's buffer, return success if
	 * anything printed.
	 */
	if ((p = __wt_strerror(error)) != NULL)
		return (p);

	if (error < 0) {
		error = __wt_map_error_to_windows_error(error);

		lasterror = FormatMessageA(
			FORMAT_MESSAGE_FROM_SYSTEM |
			    FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL,
			error,
			0, /* let system choose the correct LANGID */
			buf,
			sizeof(buf),
			NULL);

		if (lasterror != 0 &&
		    __wt_buf_set(session, &session->err, buf, strlen(buf)) == 0)
			return (session->err.data);

		/* Fall through to the fallback error code */
	}

	/* Defeated. */
	return ("Unable to return error string");
}
