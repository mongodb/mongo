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
	InitializeSRWLock(&rwlock->rwlock);

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

	rwlock->exclusive_locked = 1;
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

	rwlock->exclusive_locked = 1;

	return (0);
}

/*
 * __wt_rwunlock --
 *	Release a read/write lock.
 */
int
__wt_rwunlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	WT_RET(__wt_verbose(session, WT_VERB_MUTEX,
	    "rwlock: unlock %s (%p)", rwlock->name, rwlock));

	if (rwlock->exclusive_locked == 0)
		ReleaseSRWLockShared(&rwlock->rwlock);
	else {
		rwlock->exclusive_locked = 0;
		ReleaseSRWLockExclusive(&rwlock->rwlock);
	}
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
