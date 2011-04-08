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
__wt_mtx_alloc(SESSION *session, const char *name, int is_locked, WT_MTX **mtxp)
{
	WT_MTX *mtx;
	pthread_condattr_t condattr;
	pthread_mutexattr_t mutexattr;

	WT_RET(__wt_calloc(session, 1, sizeof(WT_MTX), &mtx));

	/*
	 * !!!
	 * This function MUST handle a NULL SESSION structure reference.
	 *
	 * Initialize the mutex.
	 * Mutexes are shared between processes.
	 */
	if (pthread_mutexattr_init(&mutexattr) != 0)
		goto err;
#if 0
	if (pthread_mutexattr_setpshared(
	    &mutexattr, PTHREAD_PROCESS_SHARED) != 0)
		goto err;
#endif
	if (pthread_mutex_init(&mtx->mtx, &mutexattr) != 0)
		goto err;
	(void)pthread_mutexattr_destroy(&mutexattr);

	/* Initialize the condition variable (mutexes are self-blocking). */
	if (pthread_condattr_init(&condattr) != 0)
		goto err;
#if 0
	if (pthread_condattr_setpshared(
	    &condattr, PTHREAD_PROCESS_SHARED) != 0)
		goto err;
#endif
	if (pthread_cond_init(&mtx->cond, &condattr) != 0)
		goto err;
	(void)pthread_condattr_destroy(&condattr);

	mtx->name = name;

	/* If the normal state of the mutex is locked, lock it immediately. */
	if (is_locked)
		__wt_lock(session, mtx);

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
__wt_lock(SESSION *session, WT_MTX *mtx)
{
	int ret;

	WT_VERBOSE(S2C(session),
	    WT_VERB_MUTEX, (session, "lock %s mutex (%p)",  mtx->name, mtx));

	WT_ERR(pthread_mutex_lock(&mtx->mtx));

	/*
	 * Check pthread_cond_wait() return for EINTR, ETIME and ETIMEDOUT,
	 * it's known to return these errors on some systems.
	 */
	while (mtx->locked) {
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

	mtx->locked = 1;
	WT_STAT_INCR(S2C(session)->stats, mtx_lock);

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
__wt_unlock(SESSION *session, WT_MTX *mtx)
{
	int ret;

	WT_VERBOSE(S2C(session),
	    WT_VERB_MUTEX, (session, "unlock %s mutex (%p)",  mtx->name, mtx));

	ret = 0;
	WT_ERR(pthread_mutex_lock(&mtx->mtx));
	mtx->locked = 0;
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
__wt_mtx_destroy(SESSION *session, WT_MTX *mtx)
{
	int ret;

	ret = pthread_cond_destroy(&mtx->cond);
	WT_TRET(pthread_mutex_destroy(&mtx->mtx));

	__wt_free(session, mtx);

	return (ret == 0 ? 0 : WT_ERROR);
}
