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
	/*
	 * Creating a thread isn't a memory barrier, but WiredTiger commonly
	 * sets flags and or state and then expects worker threads to start.
	 * Include a barrier to ensure safety in those cases.
	 */
	WT_FULL_BARRIER();

	/* Spawn a new thread of control. */
	*tidret = (HANDLE)_beginthreadex(NULL, 0, func, arg, 0, NULL);
	if (*tidret != 0)
		return (0);

	WT_RET_MSG(session, __wt_errno(), "thread create: _beginthreadex");
}

/*
 * __wt_thread_join --
 *	Wait for a thread of control to exit.
 */
int
__wt_thread_join(WT_SESSION_IMPL *session, wt_thread_t tid)
{
	DWORD windows_error;

	/*
	 * Joining a thread isn't a memory barrier, but WiredTiger commonly
	 * sets flags and or state and then expects worker threads to halt.
	 * Include a barrier to ensure safety in those cases.
	 */
	WT_FULL_BARRIER();

	if ((windows_error =
	    WaitForSingleObject(tid, INFINITE)) != WAIT_OBJECT_0) {
		if (windows_error == WAIT_FAILED)
			windows_error = __wt_getlasterror();
		__wt_errx(session, "thread join: WaitForSingleObject: %s",
		    __wt_formatmessage(session, windows_error));

		/* If we fail to wait, we will leak handles, do not continue. */
		return (WT_PANIC);
	}

	if (CloseHandle(tid) == 0) {
		windows_error = __wt_getlasterror();
		__wt_errx(session, "thread join: CloseHandle: %s",
		    __wt_formatmessage(session, windows_error));
		return (__wt_map_windows_error(windows_error));
	}

	return (0);
}

/*
 * __wt_thread_id --
 *	Fill in a printable version of the process and thread IDs.
 */
int
__wt_thread_id(char *buf, size_t buflen)
{
	return (__wt_snprintf(buf, buflen,
	    "%" PRIu64 ":%" PRIu64,
	    (uint64_t)GetCurrentProcessId(), (uint64_t)GetCurrentThreadId));
}
