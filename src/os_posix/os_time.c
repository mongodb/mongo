/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_epoch --
 *	Return the seconds and nanoseconds since the Epoch.
 */
int
__wt_epoch(WT_SESSION_IMPL *session, struct timespec *tp)
{
	int ret;

	WT_SYSCALL_RETRY(clock_gettime(CLOCK_REALTIME, tp), ret);
	if (ret == 0)
		return (0);

	WT_RET_MSG(session, ret, "clock_gettime error");
}
