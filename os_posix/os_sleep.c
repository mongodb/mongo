/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2009 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
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

	t.tv_sec = (long)seconds;
	t.tv_usec = (long)micro_seconds;

	(void)select(0, NULL, NULL, NULL, &t);
}
