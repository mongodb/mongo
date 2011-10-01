/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_ftruncate --
 *	Truncate a file.
 */
int
__wt_ftruncate(WT_SESSION_IMPL *session, WT_FH *fh, off_t len)
{
	int ret;

	SYSCALL_RETRY(ftruncate(fh->fd, len), ret);
	if (ret == 0) {
		fh->file_size = len;
		return (0);
	}

	__wt_err(session, ret, "%s ftruncate error", fh->name);
	return (ret);
}
