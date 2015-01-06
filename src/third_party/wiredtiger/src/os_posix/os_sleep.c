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
__wt_sleep(long seconds, long micro_seconds)
{
	struct timeval t;

	t.tv_sec = seconds + micro_seconds / 1000000;
	t.tv_usec = (suseconds_t)(micro_seconds % 1000000);

	(void)select(0, NULL, NULL, NULL, &t);
}
