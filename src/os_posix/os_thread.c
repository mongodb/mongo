/*-
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
    _wt_thread_t *tidret, void *(*func)(void *), void *arg)
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
__wt_thread_join(WT_SESSION_IMPL *session, _wt_thread_t tid)
{
	WT_DECL_RET;

	if ((ret = pthread_join(tid, NULL)) == 0)
		return (0);

	WT_RET_MSG(session, ret, "pthread_join");
}

/*
 * __wt_thread_id --
 *	Fill in a printable version of the process and thread IDs.
 */
void
__wt_thread_id(WT_SESSION_IMPL *session, char *buf, size_t buflen)
{
	pthread_t self;
	size_t len;

	WT_UNUSED(session);

	len = (size_t)snprintf(buf, buflen, "%" PRIu64, (uint64_t)getpid());
	if (len < buflen) {
		self = pthread_self();
		__wt_raw_to_hex_mem((const uint8_t *)&self,
		    sizeof(self), (uint8_t *)buf + len, buflen - len);
	}
}
