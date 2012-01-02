/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_filesize --
 *	Get the size of a file in bytes.
 */
int
__wt_filesize(WT_SESSION_IMPL *session, WT_FH *fh, off_t *sizep)
{
	struct stat sb;
	int ret;

	WT_VERBOSE(session, fileops, "%s: fstat", fh->name);

	WT_SYSCALL_RETRY(fstat(fh->fd, &sb), ret);
	if (ret == 0) {
		*sizep = sb.st_size;
		return (0);
	}

	WT_RET_MSG(session, ret, "%s: fstat", fh->name);
}
