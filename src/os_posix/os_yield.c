/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_yield --
 *	Yield the thread of control.
 */
void
__wt_yield(void)
{
	sched_yield();
}
