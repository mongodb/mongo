/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
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
__wt_thread_create(WT_SESSION_IMPL *session,
    pthread_t *tidret, void *(*func)(void *), void *arg)
{
	WT_DECL_RET;

	/* Spawn a new thread of control. */
	if ((ret = pthread_create(tidret, NULL, func, arg)) == 0)
		return (0);
	WT_RET_MSG(session, ret, "pthread_create");
}

/*
 * __wt_thread_join --
 *	Wait for a thread of control to exit.
 */
int
__wt_thread_join(WT_SESSION_IMPL *session, pthread_t tid)
{
	WT_DECL_RET;

#if 0 && defined(HAVE_PTHREAD_TIMEDJOIN_NP)
	struct timespec abstime;

	WT_RET(__wt_epoch(session, &abstime));
	abstime.tv_sec += 60;			/* Wait a max of 60 seconds. */
	ret = pthread_timedjoin_np(tid, NULL, &abstime);
#else
	ret = pthread_join(tid, NULL);
#endif
	if (ret == 0)
		return (0);
	WT_RET_MSG(session, ret, "pthread_join");
}
