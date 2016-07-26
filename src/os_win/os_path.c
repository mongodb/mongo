/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
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
bool
__wt_absolute_path(const char *path)
{
	/*
	 * Check for a drive name (for example, "D:"), allow both forward and
	 * backward slashes.
	 */
	if (strlen(path) >= 3 && __wt_isalpha(path[0]) && path[1] == ':')
		path += 2;
	return (path[0] == '/' || path[0] == '\\');
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
