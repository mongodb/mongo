/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_absolute_path --
 *	Return if a filename is an absolute path.
 */
int
__wt_absolute_path(const char *path)
{
	return (path[0] == '/' ? 1 : 0);
}

/*
 * __wt_path_separator --
 *	Return the path separator string.
 */
const char *
__wt_path_separator(void)
{
	return ("/");
}
