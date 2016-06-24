/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_getlasterror --
 *	Return GetLastError, or a relatively generic Windows error if the system
 * error code isn't set.
 */
DWORD
__wt_getlasterror(void)
{
	DWORD windows_error;

	/*
	 * Check for ERROR_SUCCESS:
	 * It's easy to introduce a problem by calling the wrong error function,
	 * for example, this function when the MSVC function set the C runtime
	 * error value. Handle gracefully and always return an error.
	 */
	windows_error = GetLastError();
	return (windows_error == ERROR_SUCCESS ?
	    ERROR_INVALID_PARAMETER : windows_error);
}

/*
 * __wt_map_windows_error --
 *	Map Windows errors to POSIX/ANSI errors.
 */
int
__wt_map_windows_error(DWORD windows_error)
{
	static const struct {
		int	windows_error;
		int	posix_error;
	} list[] = {
		{ ERROR_ACCESS_DENIED,		EACCES },
		{ ERROR_ALREADY_EXISTS,		EEXIST },
		{ ERROR_ARENA_TRASHED,		EFAULT },
		{ ERROR_BAD_COMMAND,		EFAULT },
		{ ERROR_BAD_ENVIRONMENT,	EFAULT },
		{ ERROR_BAD_FORMAT,		EFAULT },
		{ ERROR_BAD_NETPATH,		ENOENT },
		{ ERROR_BAD_NET_NAME,		ENOENT },
		{ ERROR_BAD_PATHNAME,		ENOENT },
		{ ERROR_BROKEN_PIPE,		EPIPE },
		{ ERROR_CANNOT_MAKE,		EACCES },
		{ ERROR_CHILD_NOT_COMPLETE,	ECHILD },
		{ ERROR_CURRENT_DIRECTORY,	EACCES },
		{ ERROR_DIRECT_ACCESS_HANDLE,	EBADF },
		{ ERROR_DIR_NOT_EMPTY,		ENOTEMPTY },
		{ ERROR_DISK_FULL,		ENOSPC },
		{ ERROR_DRIVE_LOCKED,		EACCES },
		{ ERROR_FAIL_I24,		EACCES },
		{ ERROR_FILENAME_EXCED_RANGE,	ENOENT },
		{ ERROR_FILE_EXISTS,		EEXIST },
		{ ERROR_FILE_NOT_FOUND,		ENOENT },
		{ ERROR_GEN_FAILURE,		EFAULT },
		{ ERROR_INVALID_ACCESS,		EACCES },
		{ ERROR_INVALID_BLOCK,		EFAULT },
		{ ERROR_INVALID_DATA,		EFAULT },
		{ ERROR_INVALID_DRIVE,		ENOENT },
		{ ERROR_INVALID_FUNCTION,	EINVAL },
		{ ERROR_INVALID_HANDLE,		EBADF },
		{ ERROR_INVALID_PARAMETER,	EINVAL },
		{ ERROR_INVALID_TARGET_HANDLE,	EBADF },
		{ ERROR_LOCK_FAILED,		EBUSY },
		{ ERROR_LOCK_VIOLATION,		EBUSY },
		{ ERROR_MAX_THRDS_REACHED,	EAGAIN },
		{ ERROR_NEGATIVE_SEEK,		EINVAL },
		{ ERROR_NESTING_NOT_ALLOWED,	EAGAIN },
		{ ERROR_NETWORK_ACCESS_DENIED,	EACCES },
		{ ERROR_NOT_ENOUGH_MEMORY,	ENOMEM },
		{ ERROR_NOT_ENOUGH_QUOTA,	ENOMEM },
		{ ERROR_NOT_LOCKED,		EACCES },
		{ ERROR_NOT_READY,		EBUSY },
		{ ERROR_NOT_SAME_DEVICE,	EXDEV },
		{ ERROR_NO_DATA,		EPIPE },
		{ ERROR_NO_MORE_FILES,		EMFILE },
		{ ERROR_NO_PROC_SLOTS,		EAGAIN },
		{ ERROR_PATH_NOT_FOUND,		ENOENT },
		{ ERROR_READ_FAULT,		EFAULT },
		{ ERROR_RETRY,			EINTR },
		{ ERROR_SEEK_ON_DEVICE,		EACCES },
		{ ERROR_SHARING_VIOLATION,	EBUSY },
		{ ERROR_TOO_MANY_OPEN_FILES,	EMFILE },
		{ ERROR_WAIT_NO_CHILDREN,	ECHILD },
		{ ERROR_WRITE_FAULT,		EFAULT },
		{ ERROR_WRITE_PROTECT,		EACCES },
	};
	int i;

	for (i = 0; i < WT_ELEMENTS(list); ++i)
		if (windows_error == list[i].windows_error)
			return (list[i].posix_error);

	/* Untranslatable error, go generic. */
	return (WT_ERROR);
}

/*
 * __wt_formatmessage --
 *	Windows error formatting.
 */
const char *
__wt_formatmessage(WT_SESSION_IMPL *session, DWORD windows_error)
{
	/*
	 * !!!
	 * This function MUST handle a NULL session handle.
	 *
	 * Grow the session error buffer as necessary.
	 */
	if (session != NULL &&
	    __wt_buf_initsize(session, &session->err, 512) == 0 &&
	    FormatMessageA(
	    FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
	    NULL, windows_error,
	    0, 			/* Let system choose the correct LANGID. */
	    session->err.mem, (DWORD)512, NULL) != 0)
		return (session->err.data);

	return ("Unable to format Windows error string");
}
