/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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
    wt_thread_t *tidret, WT_THREAD_CALLBACK(*func)(void *), void *arg)
{
	WT_DECL_RET;

	/* Spawn a new thread of control. */
	WT_SYSCALL_RETRY(pthread_create(tidret, NULL, func, arg), ret);
	if (ret == 0)
		return (0);
	WT_RET_MSG(session, ret, "pthread_create");
}

/*
 * __wt_thread_join --
 *	Wait for a thread of control to exit.
 */
int
__wt_thread_join(WT_SESSION_IMPL *session, wt_thread_t tid)
{
	WT_DECL_RET;

	WT_SYSCALL(pthread_join(tid, NULL), ret);
	if (ret == 0)
		return (0);

	WT_RET_MSG(session, ret, "pthread_join");
}

/*
 * __wt_thread_id --
 *	Fill in a printable version of the process and thread IDs.
 */
void
__wt_thread_id(char *buf, size_t buflen)
{
	pthread_t self;

	/*
	 * POSIX 1003.1 allows pthread_t to be an opaque type; on systems where
	 * it's a pointer, print the pointer to match gdb output.
	 */
	self = pthread_self();
#ifdef __sun
	(void)snprintf(buf, buflen,
	    "%" PRIuMAX ":%u", (uintmax_t)getpid(), self);
#else
	(void)snprintf(buf, buflen,
	    "%" PRIuMAX ":%p", (uintmax_t)getpid(), (void *)self);
#endif
}
