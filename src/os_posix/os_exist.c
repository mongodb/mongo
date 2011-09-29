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
__wt_exist(WT_SESSION_IMPL *session, const char *name)
{
	WT_BUF *tmp;
	struct stat sb;
	int ret;

	WT_RET(__wt_filename(session, name, &tmp));
	name = tmp->data;

	/*
	 * XXX
	 * This isn't correct: EINTR doesn't mean the file doesn't exist.
	 */
	SYSCALL_RETRY(stat(name, &sb), ret);

	__wt_scr_free(&tmp);

	return (ret == 0 ? 1 : 0);
}
