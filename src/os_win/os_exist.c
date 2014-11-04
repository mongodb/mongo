/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_exist --
 *	Return if the file exists.
 */
int
__wt_exist(WT_SESSION_IMPL *session, const char *filename, int *existp)
{
	WT_DECL_RET;
	char *path;

	WT_RET(__wt_filename(session, filename, &path));

	ret = GetFileAttributesA(path);

	__wt_free(session, path);

	if (ret != INVALID_FILE_ATTRIBUTES)
		*existp = 1;
	else
		*existp = 0;

	return (0);
}
