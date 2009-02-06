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
 * __wt_yield --
 *	Yield the thread of control.
 */
void
__wt_yield(void)
{
#ifdef HAVE_PTHREAD_YIELD
	pthread_yield();
#else
	sched_yield();
#endif
}
