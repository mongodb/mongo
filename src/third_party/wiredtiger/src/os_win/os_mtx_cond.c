/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_cond_alloc --
 *	Allocate and initialize a condition variable.
 */
int
__wt_cond_alloc(WT_SESSION_IMPL *session,
    const char *name, bool is_signalled, WT_CONDVAR **condp)
{
	WT_CONDVAR *cond;

	/*
	 * !!!
	 * This function MUST handle a NULL session handle.
	 */
	WT_RET(__wt_calloc_one(session, &cond));

	InitializeCriticalSection(&cond->mtx);

	/* Initialize the condition variable to permit self-blocking. */
	InitializeConditionVariable(&cond->cond);

	cond->name = name;
	cond->waiters = is_signalled ? -1 : 0;

	*condp = cond;
	return (0);
}

/*
 * __wt_cond_wait_signal --
 *	Wait on a mutex, optionally timing out.  If we get it
 *	before the time out period expires, let the caller know.
 */
int
__wt_cond_wait_signal(
    WT_SESSION_IMPL *session, WT_CONDVAR *cond, uint64_t usecs, bool *signalled)
{
	BOOL sleepret;
	DWORD milliseconds, windows_error;
	bool locked;
	uint64_t milliseconds64;

	locked = false;

	/* Fast path if already signalled. */
	*signalled = true;
	if (__wt_atomic_addi32(&cond->waiters, 1) == 0)
		return (0);

	/*
	 * !!!
	 * This function MUST handle a NULL session handle.
	 */
	if (session != NULL) {
		WT_RET(__wt_verbose(session, WT_VERB_MUTEX,
			"wait %s cond (%p)", cond->name, cond));
		WT_STAT_FAST_CONN_INCR(session, cond_wait);
	}

	EnterCriticalSection(&cond->mtx);
	locked = true;

	if (usecs > 0) {
		milliseconds64 = usecs / 1000;

		/*
		 * Check for 32-bit unsigned integer overflow
		 * INFINITE is max unsigned int on Windows
		 */
		if (milliseconds64 >= INFINITE)
			milliseconds64 = INFINITE - 1;
		milliseconds = (DWORD)milliseconds64;

		/*
		 * 0 would mean the CV sleep becomes a TryCV which we do not
		 * want
		 */
		if (milliseconds == 0)
			milliseconds = 1;

		sleepret = SleepConditionVariableCS(
		    &cond->cond, &cond->mtx, milliseconds);
	} else
		sleepret = SleepConditionVariableCS(
		    &cond->cond, &cond->mtx, INFINITE);

	/*
	 * SleepConditionVariableCS returns non-zero on success, 0 on timeout
	 * or failure.
	 */
	if (sleepret == 0) {
		windows_error = __wt_getlasterror();
		if (windows_error == ERROR_TIMEOUT) {
			*signalled = false;
			sleepret = 1;
		}
	}

	(void)__wt_atomic_subi32(&cond->waiters, 1);

	if (locked)
		LeaveCriticalSection(&cond->mtx);

	if (sleepret != 0)
		return (0);

	__wt_errx(session, "SleepConditionVariableCS: %s",
	    __wt_formatmessage(session, windows_error));
	return (__wt_map_windows_error(windows_error));
}

/*
 * __wt_cond_signal --
 *	Signal a waiting thread.
 */
int
__wt_cond_signal(WT_SESSION_IMPL *session, WT_CONDVAR *cond)
{
	WT_DECL_RET;
	bool locked;

	locked = false;

	/*
	 * !!!
	 * This function MUST handle a NULL session handle.
	 */
	if (session != NULL)
		WT_RET(__wt_verbose(session, WT_VERB_MUTEX,
			"signal %s cond (%p)", cond->name, cond));

	/* Fast path if already signalled. */
	if (cond->waiters == -1)
		return (0);

	if (cond->waiters > 0 || !__wt_atomic_casi32(&cond->waiters, 0, -1)) {
		EnterCriticalSection(&cond->mtx);
		locked = true;
		WakeAllConditionVariable(&cond->cond);
	}

	if (locked)
		LeaveCriticalSection(&cond->mtx);
	if (ret == 0)
		return (0);
	WT_RET_MSG(session, ret, "WakeAllConditionVariable");
}

/*
 * __wt_cond_destroy --
 *	Destroy a condition variable.
 */
int
__wt_cond_destroy(WT_SESSION_IMPL *session, WT_CONDVAR **condp)
{
	WT_CONDVAR *cond;
	WT_DECL_RET;

	cond = *condp;
	if (cond == NULL)
		return (0);

	/* Do nothing to delete Condition Variable */
	DeleteCriticalSection(&cond->mtx);
	__wt_free(session, *condp);

	return (ret);
}
