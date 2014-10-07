/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_ftruncate --
 *	Truncate a file.
 */
int
__wt_ftruncate(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t len)
{
	WT_DECL_RET;

	/* Ftruncate and mapped memory aren't compatible, lock. */
	__wt_spin_lock(session, &fh->lock);
	if (fh->ref_mapped == 0) {
		WT_SYSCALL_RETRY(ftruncate(fh->fd, len), ret);
		if (ret == 0)
			fh->size = fh->extend_size = len;
	} else
		ret = EBUSY;

	__wt_spin_unlock(session, &fh->lock);
	if (ret == 0 || ret == EBUSY)
		return (ret);

	WT_RET_MSG(session, ret, "%s ftruncate error", fh->name);
}
