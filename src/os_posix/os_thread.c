/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_thread_create --
 *	Create a new thread of control.
 */
int
__wt_thread_create(pthread_t *tidret, void *(*func)(void *), void *arg)
{
	/* Spawn a new thread of control. */
	return (pthread_create(tidret, NULL, func, arg));
}

/*
 * __wt_thread_join --
 *	Wait for a thread of control to exit.
 */
int
__wt_thread_join(pthread_t tid)
{
#ifdef HAVE_PTHREAD_TIMEDJOIN_NP
	struct timespec abstime;

	abstime.tv_sec = 30;			/* Wait a max of 30 seconds. */
	abstime.tv_nsec = 0;
	return (pthread_timedjoin_np(tid, NULL, &abstime));
#else
	return (pthread_join(tid, NULL));
#endif
}
