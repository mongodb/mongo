/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_exist --
 *	Return if the file exists.
 */
int
__wt_exist(WT_SESSION_IMPL *session, const char *name, int *existp)
{
	const char *path;
	struct stat sb;
	int ret;

	WT_RET(__wt_filename(session, name, &path));

	SYSCALL_RETRY(stat(path, &sb), ret);

	__wt_free(session, path);

	if (ret == 0) {
		*existp = 1;
		return (0);
	}
	if (ret == ENOENT) {
		*existp = 0;
		return (0);
	}

	__wt_err(session, ret, "%s: fstat", name);
	return (ret);
}
