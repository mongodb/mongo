/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

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
