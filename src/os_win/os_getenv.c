/*-
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
	WT_DECL_RET;
	DWORD size;

	*envp = NULL;

	size = GetEnvironmentVariableA(variable, NULL, 0);
	if (size <= 1)
		return (WT_NOTFOUND);

	WT_RET(__wt_calloc(session, 1, size, envp));

	ret = GetEnvironmentVariableA(variable, *envp, size);
	/* We expect the number of bytes not including nul terminator. */
	if ((ret + 1) != size)
		WT_RET_MSG(session, __wt_errno(),
		    "GetEnvironmentVariableA failed: %s", variable);

	return (0);
}
