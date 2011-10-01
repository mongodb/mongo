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
	WT_BUF *tmp;
	struct stat sb;
	int ret;

	WT_RET(__wt_filename(session, name, &tmp));
	name = tmp->data;

	SYSCALL_RETRY(stat(name, &sb), ret);

	__wt_scr_free(&tmp);

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
