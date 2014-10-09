/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_bytelock --
 *	Lock/unlock a byte in a file.
 */
int
__wt_bytelock(WT_FH *fhp, wt_off_t byte, int lock)
{
	WT_DECL_RET;

	/*
	 * WiredTiger requires this function be able to acquire locks past
	 * the end of file.
	 *
	 * Note we're using fcntl(2) locking: all fcntl locks associated with a
	 * file for a given process are removed when any file descriptor for the
	 * file is closed by the process, even if a lock was never requested for
	 * that file descriptor.
	 *
	 * http://msdn.microsoft.com/
	 *    en-us/library/windows/desktop/aa365202%28v=vs.85%29.aspx
	 *
	 * You can lock bytes that are beyond the end of the current file.
	 * This is useful to coordinate adding records to the end of a file.
	 */
	if (lock) {
		ret = LockFile(fhp->filehandle, UINT32_MAX & byte,
		    UINT32_MAX & (byte >> 32), 1, 0);
	} else {
		ret = UnlockFile(fhp->filehandle, UINT32_MAX & byte,
		    UINT32_MAX & (byte >> 32), 1, 0);
	}

	if (ret == FALSE)
		WT_RET_MSG(NULL, __wt_errno(), "%s: LockFile", fhp->name);

	return (0);
}
