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
	/* Spawn a new thread of control. */
	*tidret = CreateThread(NULL, 0, func, arg, 0, NULL);
	if (*tidret != NULL)
		return (0);

	WT_RET_MSG(session, __wt_errno(), "CreateThread");
}

/*
 * __wt_thread_join --
 *	Wait for a thread of control to exit.
 */
int
__wt_thread_join(WT_SESSION_IMPL *session, _wt_thread_t tid)
{
	WT_DECL_RET;

	if ((ret = WaitForSingleObject(tid, INFINITE)) == WAIT_OBJECT_0)
		return (0);

	WT_RET_MSG(session, ret, "WaitForSingleObject");
}

/*
 * __wt_thread_id --
 *	Fill in a printable version of the process and thread IDs.
 */
void
__wt_thread_id(WT_SESSION_IMPL *session, char* buf, size_t buflen)
{
	DWORD self;
	size_t len;

	WT_UNUSED(session);

	len = (size_t)snprintf(
	    buf, buflen, "%" PRIu64, (uint64_t)GetCurrentProcessId());
	if (len < buflen) {
		self = GetCurrentThreadId();
		__wt_raw_to_hex_mem((const uint8_t *)&self,
		    sizeof(self), (uint8_t *)buf + len, buflen - len);
	}
}
