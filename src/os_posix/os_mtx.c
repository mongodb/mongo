/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
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

	/* Initialize the mutex. */
	if (pthread_mutex_init(&cond->mtx, NULL) != 0)
		goto err;

	/* Initialize the condition variable to permit self-blocking. */
	if (pthread_cond_init(&cond->cond, NULL) != 0)
		goto err;

	cond->name = name;
	cond->signalled = is_signalled;

	*condp = cond;
	return (0);

err:	__wt_free(session, cond);
	return (WT_ERROR);
}

/*
 * __wt_cond_wait
 *	Wait on a mutex, optionally timing out.
 */
int
__wt_cond_wait(WT_SESSION_IMPL *session, WT_CONDVAR *cond, long usecs)
{
	struct timespec ts;
	WT_DECL_RET;
	int locked;

	locked = 0;

	/*
	 * !!!
	 * This function MUST handle a NULL session handle.
	 */
	if (session != NULL) {
		WT_VERBOSE_RET(
		    session, mutex, "wait %s cond (%p)", cond->name, cond);
		WT_CSTAT_INCR(session, cond_wait);
	}

	WT_ERR(pthread_mutex_lock(&cond->mtx));
	locked = 1;

	while (!cond->signalled) {
		if (usecs > 0) {
			WT_ERR(__wt_epoch(session, &ts));
			ts.tv_sec += (ts.tv_nsec + 1000 * usecs) / WT_BILLION;
			ts.tv_nsec = (ts.tv_nsec + 1000 * usecs) % WT_BILLION;
			ret = pthread_cond_timedwait(
			    &cond->cond, &cond->mtx, &ts);
			if (ret == ETIMEDOUT) {
				ret = 0;
				break;
			}
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
		WT_ERR(ret);
	}

	cond->signalled = 0;

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
	if (session != NULL && WT_VERBOSE_ISSET(session, mutex))
		WT_RET(__wt_verbose(
		    session, "signal %s cond (%p)", cond->name, cond));

	WT_ERR(pthread_mutex_lock(&cond->mtx));
	locked = 1;

	if (!cond->signalled) {
		cond->signalled = 1;
		WT_ERR(pthread_cond_signal(&cond->cond));
	}

err:	if (locked)
		WT_TRET(pthread_mutex_unlock(&cond->mtx));
	if (ret == 0)
		return (0);
	WT_RET_MSG(session, ret, "pthread_cond_signal");
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

	return ((ret == 0) ? 0 : WT_ERROR);

}

/*
 * __wt_rwlock_alloc --
 *	Allocate and initialize a read/write lock.
 */
int
__wt_rwlock_alloc(
    WT_SESSION_IMPL *session, const char *name, WT_RWLOCK **rwlockp)
{
	WT_DECL_RET;
	WT_RWLOCK *rwlock;

	WT_RET(__wt_calloc(session, 1, sizeof(WT_RWLOCK), &rwlock));
	WT_ERR_TEST(pthread_rwlock_init(&rwlock->rwlock, NULL), WT_ERROR);

	rwlock->name = name;
	*rwlockp = rwlock;

	WT_VERBOSE_ERR(session, mutex,
	    "rwlock: alloc %s (%p)", rwlock->name, rwlock);

	if (0) {
err:		__wt_free(session, rwlock);
	}
	return (ret);
}

/*
 * __wt_readlock
 *	Get a shared lock.
 */
int
__wt_readlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	WT_DECL_RET;

	WT_VERBOSE_RET(session, mutex,
	    "rwlock: readlock %s (%p)", rwlock->name, rwlock);
	WT_CSTAT_INCR(session, rwlock_read);

	if ((ret = pthread_rwlock_rdlock(&rwlock->rwlock)) == 0)
		return (0);
	WT_RET_MSG(session, ret, "pthread_rwlock_rdlock: %s", rwlock->name);
}

/*
 * __wt_try_writelock
 *	Try to get an exclusive lock, or fail immediately if unavailable.
 */
int
__wt_try_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	WT_DECL_RET;

	WT_VERBOSE_RET(session, mutex,
	    "rwlock: try_writelock %s (%p)", rwlock->name, rwlock);
	WT_CSTAT_INCR(session, rwlock_write);

	if ((ret =
	    pthread_rwlock_trywrlock(&rwlock->rwlock)) == 0 || ret == EBUSY)
		return (ret);
	WT_RET_MSG(session, ret, "pthread_rwlock_trywrlock: %s", rwlock->name);
}

/*
 * __wt_writelock
 *	Wait to get an exclusive lock.
 */
int
__wt_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	WT_DECL_RET;

	WT_VERBOSE_RET(session, mutex,
	    "rwlock: writelock %s (%p)", rwlock->name, rwlock);
	WT_CSTAT_INCR(session, rwlock_write);

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

	WT_VERBOSE_RET(session, mutex,
	    "rwlock: unlock %s (%p)", rwlock->name, rwlock);

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

	WT_VERBOSE_RET(session, mutex,
	    "rwlock: destroy %s (%p)", rwlock->name, rwlock);

	if ((ret = pthread_rwlock_destroy(&rwlock->rwlock)) == 0) {
		__wt_free(session, rwlock);
		return (0);
	}

	/* Deliberately leak memory on error. */
	WT_RET_MSG(session, ret, "pthread_rwlock_destroy: %s", rwlock->name);
}
