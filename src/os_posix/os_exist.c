/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
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
__wt_exist(WT_SESSION_IMPL *session, const char *filename, bool *existp)
{
	struct stat sb;
	WT_DECL_RET;
	char *path;

	*existp = false;

	WT_RET(__wt_filename(session, filename, &path));

	WT_SYSCALL_RETRY(stat(path, &sb), ret);

	__wt_free(session, path);

	if (ret == 0) {
		*existp = true;
		return (0);
	}
	if (ret == ENOENT)
		return (0);

	WT_RET_MSG(session, ret, "%s: fstat", filename);
}
