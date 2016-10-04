/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_getenv --
 * 	Get a non-NULL, greater than zero-length environment variable.
 */
int
__wt_getenv(WT_SESSION_IMPL *session, const char *variable, const char **envp)
{
	DWORD size, windows_error;

	*envp = NULL;

	if ((size = GetEnvironmentVariableA(variable, NULL, 0)) <= 1)
		return (WT_NOTFOUND);

	WT_RET(__wt_malloc(session, (size_t)size, envp));

	/* We expect the number of bytes not including nul terminator. */
	if (GetEnvironmentVariableA(variable, *envp, size) == size - 1)
		return (0);

	windows_error = __wt_getlasterror();
	__wt_errx(session,
	    "GetEnvironmentVariableA: %s: %s",
	    variable, __wt_formatmessage(session, windows_error));
	return (__wt_map_windows_error(windows_error));
}
