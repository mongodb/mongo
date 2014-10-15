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
	WT_DECL_RET;

	/*
	 * !!!
	 * This function MUST handle a NULL session handle.
	 */
	WT_RET(__wt_calloc(session, 1, sizeof(WT_CONDVAR), &cond));

	WT_ERR(pthread_mutex_init(&cond->mtx, NULL));

	/* Initialize the condition variable to permit self-blocking. */
	WT_ERR(pthread_cond_init(&cond->cond, NULL));

	cond->name = name;
	cond->waiters = is_signalled ? -1 : 0;

	*condp = cond;
	return (0);

err:	__wt_free(session, cond);
	return (ret);
}

/*
 * __wt_cond_wait --
 *	Wait on a mutex, optionally timing out.
 */
int
__wt_cond_wait(WT_SESSION_IMPL *session, WT_CONDVAR *cond, long usecs)
{
	struct timespec ts;
	WT_DECL_RET;
	int locked;

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

	WT_ERR(pthread_mutex_lock(&cond->mtx));
	locked = 1;

	if (usecs > 0) {
		WT_ERR(__wt_epoch(session, &ts));
		ts.tv_sec += (ts.tv_nsec + 1000 * usecs) / WT_BILLION;
		ts.tv_nsec = (ts.tv_nsec + 1000 * usecs) % WT_BILLION;
		ret = pthread_cond_timedwait(&cond->cond, &cond->mtx, &ts);
	} else
		ret = pthread_cond_wait(&cond->cond, &cond->mtx);

	/*
	 * Check pthread_cond_wait() return for EINTR, ETIME and
	 * ETIMEDOUT, some systems return these errors.
	 */
	if (ret == EINTR ||
#ifdef ETIME
	    ret == ETIME ||
#endif
	    ret == ETIMEDOUT)
		ret = 0;

	(void)WT_ATOMIC_SUB4(cond->waiters, 1);

err:	if (locked)
		WT_TRET(pthread_mutex_unlock(&cond->mtx));
	if (ret == 0)
		return (0);
	WT_RET_MSG(session, ret, "pthread_cond_wait");
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
		WT_ERR(pthread_mutex_lock(&cond->mtx));
		locked = 1;
		WT_ERR(pthread_cond_broadcast(&cond->cond));
	}

err:	if (locked)
		WT_TRET(pthread_mutex_unlock(&cond->mtx));
	if (ret == 0)
		return (0);
	WT_RET_MSG(session, ret, "pthread_cond_broadcast");
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

	ret = pthread_cond_destroy(&cond->cond);
	WT_TRET(pthread_mutex_destroy(&cond->mtx));
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
	WT_ERR(pthread_rwlock_init(&rwlock->rwlock, NULL));

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
	WT_DECL_RET;

	WT_RET(__wt_verbose(session, WT_VERB_MUTEX,
	    "rwlock: readlock %s (%p)", rwlock->name, rwlock));
	WT_STAT_FAST_CONN_INCR(session, rwlock_read);

	/*
	 * The read-lock call can fail with EAGAIN under load:
	 * retry in that case.
	 */
	WT_SYSCALL_RETRY(pthread_rwlock_rdlock(&rwlock->rwlock), ret);
	if (ret == 0)
		return (0);
	WT_RET_MSG(session, ret, "pthread_rwlock_rdlock: %s", rwlock->name);
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

	if ((ret =
	    pthread_rwlock_trywrlock(&rwlock->rwlock)) == 0 || ret == EBUSY)
		return (ret);
	WT_RET_MSG(session, ret, "pthread_rwlock_trywrlock: %s", rwlock->name);
}

/*
 * __wt_writelock --
 *	Wait to get an exclusive lock.
 */
int
__wt_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	WT_DECL_RET;

	WT_RET(__wt_verbose(session, WT_VERB_MUTEX,
	    "rwlock: writelock %s (%p)", rwlock->name, rwlock));
	WT_STAT_FAST_CONN_INCR(session, rwlock_write);

	if ((ret = pthread_rwlock_wrlock(&rwlock->rwlock)) == 0)
		return (0);
	WT_RET_MSG(session, ret, "pthread_rwlock_wrlock: %s", rwlock->name);
}

/*
 * __wt_rwunlock --
 *	Release a read/write lock.
 */
int
__wt_rwunlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	WT_DECL_RET;

	WT_RET(__wt_verbose(session, WT_VERB_MUTEX,
	    "rwlock: unlock %s (%p)", rwlock->name, rwlock));

	if ((ret = pthread_rwlock_unlock(&rwlock->rwlock)) == 0)
		return (0);
	WT_RET_MSG(session, ret, "pthread_rwlock_unlock: %s", rwlock->name);
}

/*
 * __wt_rwlock_destroy --
 *	Destroy a mutex.
 */
int
__wt_rwlock_destroy(WT_SESSION_IMPL *session, WT_RWLOCK **rwlockp)
{
	WT_DECL_RET;
	WT_RWLOCK *rwlock;

	rwlock = *rwlockp;		/* Clear our caller's reference. */
	if (rwlock == NULL)
		return (0);
	*rwlockp = NULL;

	WT_RET(__wt_verbose(session, WT_VERB_MUTEX,
	    "rwlock: destroy %s (%p)", rwlock->name, rwlock));

	if ((ret = pthread_rwlock_destroy(&rwlock->rwlock)) == 0) {
		__wt_free(session, rwlock);
		return (0);
	}

	/* Deliberately leak memory on error. */
	WT_RET_MSG(session, ret, "pthread_rwlock_destroy: %s", rwlock->name);
}
