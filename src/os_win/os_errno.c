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
 * __wt_strerror_r --
 *	Windows implementation of wiredtiger_strerror_r.
 */
int
__wt_strerror_r(int error, char *buf, size_t buflen)
{
	DWORD lasterror;
	const char *p;

	/* Require at least 2 bytes, printable character and trailing nul. */
	if (buflen < 2)
		return (ENOMEM);

	/*
	 * Check for POSIX errors, Windows errors, then fallback to something
	 * generic.  Copy the string into the user's buffer, return success if
	 * anything printed.
	 */
	p = __wt_strerror(error);
	if (p != NULL && snprintf(buf, buflen, "%s", p) > 0)
		return (0);

	if (error < 0) {
		error = __wt_map_error_to_windows_error(error);

		lasterror = FormatMessageA(
			FORMAT_MESSAGE_FROM_SYSTEM |
			    FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL,
			error,
			0, /* let system choose the correct LANGID */
			buf,
			buflen,
			NULL);

		if (lasterror != 0)
			return (0);

		/* Fall through to the fallback error code */
	}

	/* Fallback to a generic message, then guess it's a memory problem. */
	return (
	    snprintf(buf, buflen, "error return: %d", error) > 0 ? 0 : ENOMEM);
}
