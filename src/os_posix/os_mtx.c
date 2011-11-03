/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_mtx_alloc --
 *	Allocate and initialize a pthread mutex.
 */
int
__wt_mtx_alloc(WT_SESSION_IMPL *session,
    const char *name, int is_locked, WT_MTX **mtxp)
{
	WT_MTX *mtx;
	pthread_condattr_t condattr;
	pthread_mutexattr_t mutexattr;

	/*
	 * !!!
	 * This function MUST handle a NULL WT_SESSION_IMPL structure reference.
	 */
	WT_RET(__wt_calloc(session, 1, sizeof(WT_MTX), &mtx));

	/*
	 * Initialize the mutex.
	 * Mutexes are shared between processes.
	 */
	if (pthread_mutexattr_init(&mutexattr) != 0)
		goto err;
	if (pthread_mutex_init(&mtx->mtx, &mutexattr) != 0)
		goto err;
	(void)pthread_mutexattr_destroy(&mutexattr);

	/* Initialize the condition variable (mutexes are self-blocking). */
	if (pthread_condattr_init(&condattr) != 0)
		goto err;
	if (pthread_cond_init(&mtx->cond, &condattr) != 0)
		goto err;
	(void)pthread_condattr_destroy(&condattr);

	mtx->name = name;
	mtx->locked = is_locked;

	*mtxp = mtx;
	return (0);

err:	__wt_free(session, mtx);
	return (WT_ERROR);
}

/*
 * __wt_lock
 *	Lock a mutex.
 */
void
__wt_lock(WT_SESSION_IMPL *session, WT_MTX *mtx)
{
	int ret;

	/*
	 * !!!
	 * This function MUST handle a NULL WT_SESSION_IMPL structure reference.
	 */
	if (session != NULL)
		WT_VERBOSE(
		    session, MUTEX, "lock %s mutex (%p)",  mtx->name, mtx);

	WT_ERR(pthread_mutex_lock(&mtx->mtx));

	/*
	 * Check pthread_cond_wait() return for EINTR, ETIME and ETIMEDOUT,
	 * it's known to return these errors on some systems.
	 */
	while (mtx->locked > 0) {
		ret = pthread_cond_wait(&mtx->cond, &mtx->mtx);
		if (ret != 0 &&
		    ret != EINTR &&
#ifdef ETIME
		    ret != ETIME &&
#endif
		    ret != ETIMEDOUT) {
			(void)pthread_mutex_unlock(&mtx->mtx);
			goto err;
		}
	}

	++mtx->locked;
	if (session != NULL)
		WT_CSTAT_INCR(session, mtx_lock);

	WT_ERR(pthread_mutex_unlock(&mtx->mtx));
	return;

err:	__wt_err(session, ret, "mutex lock failed");
	__wt_abort(session);
}

/*
 * __wt_unlock --
 *	Release a mutex.
 */
void
__wt_unlock(WT_SESSION_IMPL *session, WT_MTX *mtx)
{
	int ret;

	/*
	 * !!!
	 * This function MUST handle a NULL WT_SESSION_IMPL structure reference.
	 */
	if (session != NULL)
		WT_VERBOSE(
		    session, MUTEX, "unlock %s mutex (%p)",  mtx->name, mtx);

	ret = 0;
	WT_ERR(pthread_mutex_lock(&mtx->mtx));
	if (--mtx->locked == 0)
		WT_ERR(pthread_cond_signal(&mtx->cond));
	WT_ERR(pthread_mutex_unlock(&mtx->mtx));
	return;

err:	__wt_err(session, ret, "mutex unlock failed");
	__wt_abort(session);
}

/*
 * __wt_mtx_destroy --
 *	Destroy a mutex.
 */
int
__wt_mtx_destroy(WT_SESSION_IMPL *session, WT_MTX *mtx)
{
	int ret;

	ret = pthread_cond_destroy(&mtx->cond);
	WT_TRET(pthread_mutex_destroy(&mtx->mtx));

	__wt_free(session, mtx);

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
	WT_RWLOCK *rwlock;
	int ret;

	WT_RET(__wt_calloc(session, 1, sizeof(WT_RWLOCK), &rwlock));
	ret = 0;
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
	int ret;

	WT_VERBOSE(session, MUTEX,
	    "readlock %s rwlock (%p)",  rwlock->name, rwlock);

	WT_ERR(pthread_rwlock_rdlock(&rwlock->rwlock));
	WT_CSTAT_INCR(session, rwlock_rdlock);
	return;

err:	__wt_err(session, ret, "rwlock readlock failed");
	__wt_abort(session);
}

/*
 * __wt_writelock
 *	Get an exclusive lock.
 */
void
__wt_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	int ret;

	WT_VERBOSE(session, MUTEX,
	    "writelock %s rwlock (%p)",  rwlock->name, rwlock);

	WT_ERR(pthread_rwlock_wrlock(&rwlock->rwlock));
	WT_CSTAT_INCR(session, rwlock_rdlock);
	return;

err:	__wt_err(session, ret, "rwlock writelock failed");
	__wt_abort(session);
}

/*
 * __wt_rwunlock --
 *	Release a read/write lock.
 */
void
__wt_rwunlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	int ret;

	WT_VERBOSE(session, MUTEX,
	    "unlock %s rwlock (%p)",  rwlock->name, rwlock);

	WT_ERR(pthread_rwlock_unlock(&rwlock->rwlock));
	return;

err:	__wt_err(session, ret, "rwlock unlock failed");
	__wt_abort(session);
}

/*
 * __wt_rwlock_destroy --
 *	Destroy a mutex.
 */
int
__wt_rwlock_destroy(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	int ret;

	ret = pthread_rwlock_destroy(&rwlock->rwlock);
	if (ret == EBUSY)
		ret = 0;
	WT_ASSERT(session, ret == 0);
	__wt_free(session, rwlock);

	return (ret);
}
