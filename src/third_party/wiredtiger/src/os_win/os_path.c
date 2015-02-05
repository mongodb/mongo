/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
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
	/*
	 * Check for a drive name (for example, "D:"), allow both forward and
	 * backward slashes.
	 */
	if (strlen(path) >= 3 && isalpha(path[0]) && path[1] == ':')
		path += 2;
	return (path[0] == '/' || path[0] == '\\' ? 1 : 0);
}

/*
 * __wt_path_separator --
 *	Return the path separator string.
 */
const char *
__wt_path_separator(void)
{
	return ("\\");
}
