/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_getenv --
 * 	Get a non-null environment variable
 */
int
__wt_getenv(WT_SESSION_IMPL *session, const char *variable, const char **env)
{
	const char *temp;

	*env = NULL;

	if (((temp = getenv(variable)) != NULL) && strlen(temp) > 0) {
		return (__wt_strdup(session, temp, env));
	}

	return (WT_NOTFOUND);
}
