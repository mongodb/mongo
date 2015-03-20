/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_sleep --
 *	Pause the thread of control.
 */
void
__wt_sleep(uint64_t seconds, uint64_t micro_seconds)
{
	/*
	 * If the caller wants a small pause, set to our
	 * smallest granularity.
	 */
	if (seconds == 0 && micro_seconds < 1000)
		micro_seconds = 1000;
	Sleep(seconds * 1000 + micro_seconds / 1000);
}
