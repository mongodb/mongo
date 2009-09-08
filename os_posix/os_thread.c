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
 * __wt_thread_create --
 *	Create a new thread of control.
 */
int
__wt_thread_create(
    ENV *env, pthread_t *tidret, void *(*func)(void *), void *arg)
{
	/* Spawn a new thread of control. */
	if (pthread_create(tidret, NULL, func, arg) != 0) {
		__wt_env_err(env, errno, "thread creation");
		return (WT_ERROR);
	}
	return (0);
}

/*
 * __wt_thread_join --
 *	Wait for a thread of control to exit.
 */
void
__wt_thread_join(pthread_t tid)
{
	(void)pthread_join(tid, NULL);
}
