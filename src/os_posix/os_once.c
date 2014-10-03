/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *  All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_once --
 *  One-time initialization per process.
 */
int
__wt_once(void (*init_routine)(void))
{
	static pthread_once_t once_control = PTHREAD_ONCE_INIT;

	/*
	 * Do per-process initialization once, before anything else, but only
	 * once.  I don't know how heavy_weight pthread_once might be, so I'm
	 * front-ending it with a local static and only using pthread_once to
	 * avoid a race.
	 */
	return pthread_once(&once_control, init_routine);
}
