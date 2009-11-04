/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_mtx_init --
 *	Initialize a pthread mutex.
 */
int
__wt_mtx_init(WT_MTX *mtx)
{
	pthread_condattr_t condattr;
	pthread_mutexattr_t mutexattr;

	/* Start unlocked. */
	mtx->locked = 0;

	/*
	 * !!!
	 * This function MUST handle a NULL ENV structure reference.
	 *
	 * Initialize the mutex.
	 * Mutexes are shared between processes.
	 */
	if (pthread_mutexattr_init(&mutexattr) != 0)
		return (WT_ERROR);
#if 0
	if (pthread_mutexattr_setpshared(
	    &mutexattr, PTHREAD_PROCESS_SHARED) != 0)
		return (WT_ERROR);
#endif
	if (pthread_mutex_init(&mtx->mtx, &mutexattr) != 0)
		return (WT_ERROR);
	(void)pthread_mutexattr_destroy(&mutexattr);

	/* Initialize the condition variable (mutexes are self-blocking). */
	if (pthread_condattr_init(&condattr) != 0)
		return (WT_ERROR);
#if 0
	if (pthread_condattr_setpshared(
	    &condattr, PTHREAD_PROCESS_SHARED) != 0)
		return (WT_ERROR);
#endif
	if (pthread_cond_init(&mtx->cond, &condattr) != 0)
		return (WT_ERROR);
	(void)pthread_condattr_destroy(&condattr);

	return (0);
}

/*
 * __wt_lock
 *	Lock a mutex.
 */
void
__wt_lock(ENV *env, WT_MTX *mtx)
{
	int ret;

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
	WT_STAT_INCR(env->ienv->stats, MTX_LOCK, "mutex lock calls");

	WT_ERR(pthread_mutex_unlock(&mtx->mtx));
	return;

err:	__wt_api_env_err(env, ret, "mutex lock failed");
	__wt_abort();
}

/*
 * __wt_unlock --
 *	Release a mutex.
 */
void
__wt_unlock(WT_MTX *mtx)
{
	int ret;

	ret = 0;
	WT_ERR(pthread_mutex_lock(&mtx->mtx));
	mtx->locked = 0;
	WT_ERR(pthread_cond_signal(&mtx->cond));

	WT_ERR(pthread_mutex_unlock(&mtx->mtx));
	return;

err:	__wt_api_env_err(NULL, ret, "mutex unlock failed");
	__wt_abort();
}

/*
 * __wt_mtx_destroy --
 *	Destroy a mutex.
 */
int
__wt_mtx_destroy(WT_MTX *mtx)
{
	int ret;

	ret = pthread_cond_destroy(&mtx->cond);

	WT_TRET(pthread_mutex_destroy(&mtx->mtx));

	return (ret == 0 ? 0 : WT_ERROR);
}
