/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
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
    const char *name, int is_locked, WT_CONDVAR **condp)
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
	cond->locked = is_locked;

	*condp = cond;
	return (0);

err:	__wt_free(session, cond);
	return (WT_ERROR);
}

/*
 * __wt_lock
 *	Lock a mutex.
 */
void
__wt_cond_wait(WT_SESSION_IMPL *session, WT_CONDVAR *cond)
{
	WT_DECL_RET;

	/*
	 * !!!
	 * This function MUST handle a NULL session handle.
	 */
	if (session != NULL)
		WT_VERBOSE_VOID(
		    session, mutex, "lock %s mutex (%p)", cond->name, cond);

	WT_ERR(pthread_mutex_lock(&cond->mtx));

	/*
	 * Check pthread_cond_wait() return for EINTR, ETIME and ETIMEDOUT,
	 * it's known to return these errors on some systems.
	 */
	while (cond->locked) {
		ret = pthread_cond_wait(&cond->cond, &cond->mtx);
		if (ret != 0 &&
		    ret != EINTR &&
#ifdef ETIME
		    ret != ETIME &&
#endif
		    ret != ETIMEDOUT) {
			(void)pthread_mutex_unlock(&cond->mtx);
			goto err;
		}
	}

	cond->locked = 1;
	if (session != NULL)
		WT_CSTAT_INCR(session, cond_wait);

	WT_ERR(pthread_mutex_unlock(&cond->mtx));
	return;

err:	__wt_err(session, ret, "mutex lock failed");
	__wt_abort(session);
}

/*
 * __wt_cond_signal --
 *	Signal a waiting thread.
 */
void
__wt_cond_signal(WT_SESSION_IMPL *session, WT_CONDVAR *cond)
{
	WT_DECL_RET;

	/*
	 * !!!
	 * This function MUST handle a NULL session handle.
	 */
	if (session != NULL)
		WT_VERBOSE_VOID(
		    session, mutex, "signal %s cond (%p)", cond->name, cond);

	WT_ERR(pthread_mutex_lock(&cond->mtx));
	if (cond->locked) {
		cond->locked = 0;
		WT_ERR(pthread_cond_signal(&cond->cond));
	}
	WT_ERR(pthread_mutex_unlock(&cond->mtx));
	return;

err:	__wt_err(session, ret, "mutex unlock failed");
	__wt_abort(session);
}

/*
 * __wt_cond_destroy --
 *	Destroy a condition variable.
 */
int
__wt_cond_destroy(WT_SESSION_IMPL *session, WT_CONDVAR *cond)
{
	WT_DECL_RET;

	ret = pthread_cond_destroy(&cond->cond);
	WT_TRET(pthread_mutex_destroy(&cond->mtx));

	__wt_free(session, cond);

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
	if (0) {
err:		__wt_free(session, rwlock);
	}
	return (ret);
}

/*
 * __wt_readlock
 *	Get a shared lock.
 */
void
__wt_readlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	WT_DECL_RET;

	WT_VERBOSE_VOID(session, mutex,
	    "readlock %s rwlock (%p)", rwlock->name, rwlock);

	WT_ERR(pthread_rwlock_rdlock(&rwlock->rwlock));
	WT_CSTAT_INCR(session, rwlock_rdlock);

	if (0) {
err:		__wt_err(session, ret, "rwlock readlock failed");
		__wt_abort(session);
	}
}

/*
 * __wt_try_writelock
 *	Try to get an exclusive lock, or fail immediately if unavailable.
 */
int
__wt_try_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	WT_DECL_RET;

	WT_VERBOSE_VOID(session, mutex,
	    "try_writelock %s rwlock (%p)", rwlock->name, rwlock);

	if ((ret = pthread_rwlock_trywrlock(&rwlock->rwlock)) == 0)
		WT_CSTAT_INCR(session, rwlock_wrlock);
	else if (ret != EBUSY) {
		__wt_err(session, ret, "rwlock try_writelock failed");
		__wt_abort(session);
	}

	return (ret);
}

/*
 * __wt_writelock
 *	Wait to get an exclusive lock.
 */
void
__wt_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	WT_DECL_RET;

	WT_VERBOSE_VOID(session, mutex,
	    "writelock %s rwlock (%p)", rwlock->name, rwlock);

	WT_ERR(pthread_rwlock_wrlock(&rwlock->rwlock));
	WT_CSTAT_INCR(session, rwlock_wrlock);

	if (0) {
err:		__wt_err(session, ret, "rwlock writelock failed");
		__wt_abort(session);
	}
}

/*
 * __wt_rwunlock --
 *	Release a read/write lock.
 */
void
__wt_rwunlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	WT_DECL_RET;

	WT_VERBOSE_VOID(session, mutex,
	    "unlock %s rwlock (%p)", rwlock->name, rwlock);

	WT_ERR(pthread_rwlock_unlock(&rwlock->rwlock));

	if (0) {
err:		__wt_err(session, ret, "rwlock unlock failed");
		__wt_abort(session);
	}
}

/*
 * __wt_rwlock_destroy --
 *	Destroy a mutex.
 */
int
__wt_rwlock_destroy(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	WT_DECL_RET;

	ret = pthread_rwlock_destroy(&rwlock->rwlock);
	if (ret == EBUSY)
		ret = 0;
	WT_ASSERT(session, ret == 0);
	__wt_free(session, rwlock);

	return (ret);
}
