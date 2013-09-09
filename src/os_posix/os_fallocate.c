/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_fallocate --
 *	Allocate space for a file handle.
 */
int
__wt_fallocate(WT_SESSION_IMPL *session, WT_FH *fh, off_t offset, off_t len)
{
	WT_DECL_RET;

	WT_VERBOSE_RET(session, fileops, "%s: fallocate", fh->name);

#if defined(HAVE_POSIX_FALLOCATE)
	WT_SYSCALL_RETRY(posix_fallocate(fh->fd, offset, len), ret);
	if (ret != 0)
		WT_RET_MSG(session, ret, "%s: posix_fallocate", fh->name);
#elif defined(HAVE_FTRUNCATE)
	WT_SYSCALL_RETRY(ftruncate(fh->fd, offset + len), ret);
	if (ret != 0)
		WT_RET_MSG(session, ret, "%s: ftruncate", fh->name);
#else
	WT_UNUSED(ret);
	WT_UNUSED(offset);
	WT_UNUSED(len);
#endif
	return (0);
}
