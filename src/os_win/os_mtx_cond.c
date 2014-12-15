/*-
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
    const char *name, int is_signalled, WT_CONDVAR **condp)
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
 * __wt_cond_wait --
 *	Wait on a mutex, optionally timing out.
 */
int
__wt_cond_wait(WT_SESSION_IMPL *session, WT_CONDVAR *cond, long usecs)
{
	WT_DECL_RET;
	int locked;
	int lasterror;
	int milliseconds;
	locked = 0;
	WT_ASSERT(session, usecs >= 0);

	/* Fast path if already signalled. */
	if (WT_ATOMIC_ADD4(cond->waiters, 1) == 0)
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
	locked = 1;

	if (usecs > 0) {
		milliseconds = usecs / 1000;
		/*
		 * 0 would mean the CV sleep becomes a TryCV which we do not
		 * want
		 */
		if (milliseconds == 0)
			milliseconds = 1;
		ret = SleepConditionVariableCS(
		    &cond->cond, &cond->mtx, milliseconds);
	} else
		ret = SleepConditionVariableCS(
		    &cond->cond, &cond->mtx, INFINITE);

	if (ret == 0) {
		lasterror = GetLastError();
		if (lasterror == ERROR_TIMEOUT) {
			ret = 1;
		}
	}

	(void)WT_ATOMIC_SUB4(cond->waiters, 1);

	if (locked)
		LeaveCriticalSection(&cond->mtx);
	if (ret != 0)
		return (0);
	WT_RET_MSG(session, ret, "SleepConditionVariableCS");
}

/*
 * __wt_cond_signal --
 *	Signal a waiting thread.
 */
int
__wt_cond_signal(WT_SESSION_IMPL *session, WT_CONDVAR *cond)
{
	WT_DECL_RET;
	int locked;

	locked = 0;

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

	if (cond->waiters > 0 || !WT_ATOMIC_CAS4(cond->waiters, 0, -1)) {
		EnterCriticalSection(&cond->mtx);
		locked = 1;
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
