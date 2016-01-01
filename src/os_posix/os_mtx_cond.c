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
	WT_DECL_RET;

	/*
	 * !!!
	 * This function MUST handle a NULL session handle.
	 */
	WT_RET(__wt_calloc_one(session, &cond));

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
 * __wt_cond_wait_signal --
 *	Wait on a mutex, optionally timing out.  If we get it
 *	before the time out period expires, let the caller know.
 */
int
__wt_cond_wait_signal(
    WT_SESSION_IMPL *session, WT_CONDVAR *cond, uint64_t usecs, bool *signalled)
{
	struct timespec ts;
	WT_DECL_RET;
	bool locked;

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

	WT_ERR(pthread_mutex_lock(&cond->mtx));
	locked = true;

	if (usecs > 0) {
		WT_ERR(__wt_epoch(session, &ts));
		ts.tv_sec += (time_t)
		    (((uint64_t)ts.tv_nsec + WT_THOUSAND * usecs) / WT_BILLION);
		ts.tv_nsec = (long)
		    (((uint64_t)ts.tv_nsec + WT_THOUSAND * usecs) % WT_BILLION);
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
	    ret == ETIMEDOUT) {
		*signalled = false;
		ret = 0;
	}

	(void)__wt_atomic_subi32(&cond->waiters, 1);

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
		WT_ERR(pthread_mutex_lock(&cond->mtx));
		locked = true;
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
