/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
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
	WT_DECL_RET;

	WT_VERBOSE_RET(session, fileops, "%s: fstat", fh->name);

	WT_SYSCALL_RETRY(fstat(fh->fd, &sb), ret);
	if (ret == 0) {
		*sizep = sb.st_size;
		return (0);
	}

	WT_RET_MSG(session, ret, "%s: fstat", fh->name);
}
