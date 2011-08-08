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

	WT_VERBOSE(session, FILEOPS, "fileops: %s: fstat", fh->name);

	SYSCALL_RETRY(fstat(fh->fd, &sb), ret);
	if (ret == 0) {
		*sizep = sb.st_size;
		return (0);
	}
	__wt_err(session, errno, "%s: fstat", fh->name);
	return (WT_ERROR);
}
