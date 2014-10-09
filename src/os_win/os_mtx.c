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
	WT_RET(__wt_calloc(session, 1, sizeof(WT_CONDVAR), &cond));

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

/*
 * __wt_rwlock_alloc --
 *	Allocate and initialize a read/write lock.
 */
int
__wt_rwlock_alloc(
    WT_SESSION_IMPL *session, WT_RWLOCK **rwlockp, const char *name)
{
	WT_DECL_RET;
	WT_RWLOCK *rwlock;

	WT_RET(__wt_calloc(session, 1, sizeof(WT_RWLOCK), &rwlock));
	InitializeSRWLock(&rwlock->rwlock);

	rwlock->exclusive_locked = 0;
	rwlock->name = name;
	*rwlockp = rwlock;

	WT_ERR(__wt_verbose(session, WT_VERB_MUTEX,
	    "rwlock: alloc %s (%p)", rwlock->name, rwlock));

	if (0) {
err:		__wt_free(session, rwlock);
	}
	return (ret);
}

/*
 * __wt_readlock --
 *	Get a shared lock.
 */
int
__wt_readlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	WT_RET(__wt_verbose(session, WT_VERB_MUTEX,
	    "rwlock: readlock %s (%p)", rwlock->name, rwlock));
	WT_STAT_FAST_CONN_INCR(session, rwlock_read);

	AcquireSRWLockShared(&rwlock->rwlock);

	return (0);
}

/*
 * __wt_try_writelock --
 *	Try to get an exclusive lock, or fail immediately if unavailable.
 */
int
__wt_try_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	WT_DECL_RET;

	WT_RET(__wt_verbose(session, WT_VERB_MUTEX,
	    "rwlock: try_writelock %s (%p)", rwlock->name, rwlock));
	WT_STAT_FAST_CONN_INCR(session, rwlock_write);

	ret = TryAcquireSRWLockExclusive(&rwlock->rwlock);
	if (ret == 0)
		return (EBUSY);

	rwlock->exclusive_locked = GetCurrentThreadId();
	return (0);
}

/*
 * __wt_writelock --
 *	Wait to get an exclusive lock.
 */
int
__wt_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	WT_RET(__wt_verbose(session, WT_VERB_MUTEX,
	    "rwlock: writelock %s (%p)", rwlock->name, rwlock));
	WT_STAT_FAST_CONN_INCR(session, rwlock_write);

	AcquireSRWLockExclusive(&rwlock->rwlock);

	rwlock->exclusive_locked = GetCurrentThreadId();

	return (0);
}

/*
 * __wt_rwunlock --
 *	Release a read/write lock.
 */
int
__wt_rwunlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	if (rwlock->exclusive_locked != 0) {
		rwlock->exclusive_locked = 0;
		ReleaseSRWLockExclusive(&rwlock->rwlock);
	} else
		ReleaseSRWLockShared(&rwlock->rwlock);

	WT_RET(__wt_verbose(session, WT_VERB_MUTEX,
	    "rwlock: unlock %s (%p)", rwlock->name, rwlock));

	return (0);
}

/*
 * __wt_rwlock_destroy --
 *	Destroy a mutex.
 */
int
__wt_rwlock_destroy(WT_SESSION_IMPL *session, WT_RWLOCK **rwlockp)
{
	WT_RWLOCK *rwlock;

	rwlock = *rwlockp;		/* Clear our caller's reference. */
	if (rwlock == NULL)
		return (0);

	/* Nothing to delete for Slim Reader Writer lock */
	*rwlockp = NULL;

	WT_RET(__wt_verbose(session, WT_VERB_MUTEX,
	    "rwlock: destroy %s (%p)", rwlock->name, rwlock));

	__wt_free(session, rwlock);
	return (0);
}
