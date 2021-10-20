/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_getenv --
 *     Get a non-NULL, greater than zero-length environment variable.
 */
int
__wt_getenv(WT_SESSION_IMPL *session, const char *variable, const char **envp)
  WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    const char *temp;

    *envp = NULL;

    if (((temp = getenv(variable)) != NULL) && strlen(temp) > 0)
        return (__wt_strdup(session, temp, envp));

    return (0);
}
