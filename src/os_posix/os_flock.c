/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_bytelock --
 *	Lock/unlock a byte in a file.
 */
int
__wt_bytelock(WT_FH *fhp, off_t byte, int lock)
{
	struct flock fl;
	int ret;

	/*
	 * WiredTiger requires this function be able to acquire locks past
	 * the end of file.
	 *
	 * Note we're using fcntl(2) locking: all fcntl locks associated with a
	 * file for a given process are removed when any file descriptor for the
	 * file is closed by the process, even if a lock was never requested for
	 * that file descriptor.
	 */
	fl.l_start = byte;
	fl.l_len = 1;
	fl.l_type = lock ? F_WRLCK : F_UNLCK;
	fl.l_whence = SEEK_SET;

	SYSCALL_RETRY(fcntl(fhp->fd, F_SETLK, &fl), ret);

	return (ret == 0 ? 0 : errno);
}
