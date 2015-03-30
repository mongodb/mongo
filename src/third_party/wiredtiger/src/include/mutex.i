/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Spin locks:
 *
 * These used for cases where fast mutual exclusion is needed (where operations
 * done while holding the spin lock are expected to complete in a small number
 * of instructions.
 */

#if SPINLOCK_TYPE == SPINLOCK_GCC

#define	WT_DECL_SPINLOCK_ID(i)
#define	__wt_spin_trylock(session, lock, idp)				\
	__wt_spin_trylock_func(session, lock)

/* Default to spinning 1000 times before yielding. */
#ifndef WT_SPIN_COUNT
#define	WT_SPIN_COUNT 1000
#endif

/*
 * __wt_spin_init --
 *      Initialize a spinlock.
 */
static inline int
__wt_spin_init(WT_SESSION_IMPL *session, WT_SPINLOCK *t, const char *name)
{
	WT_UNUSED(session);
	WT_UNUSED(name);

	*(t) = 0;
	return (0);
}

/*
 * __wt_spin_destroy --
 *      Destroy a spinlock.
 */
static inline void
__wt_spin_destroy(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	*(t) = 0;
}

/*
 * __wt_spin_trylock_func --
 *      Try to lock a spinlock or fail immediately if it is busy.
 */
static inline int
__wt_spin_trylock_func(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	return (__sync_lock_test_and_set(t, 1) == 0 ? 0 : EBUSY);
}

/*
 * __wt_spin_lock --
 *      Spin until the lock is acquired.
 */
static inline void
__wt_spin_lock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	int i;

	WT_UNUSED(session);

	while (__sync_lock_test_and_set(t, 1)) {
		for (i = 0; *t && i < WT_SPIN_COUNT; i++)
			WT_PAUSE();
		if (*t)
			__wt_yield();
	}
}

/*
 * __wt_spin_unlock --
 *      Release the spinlock.
 */
static inline void
__wt_spin_unlock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	__sync_lock_release(t);
}

#elif SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX ||\
	SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX_ADAPTIVE ||\
	SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX_LOGGING

/*
 * __wt_spin_init --
 *      Initialize a spinlock.
 */
static inline int
__wt_spin_init(WT_SESSION_IMPL *session, WT_SPINLOCK *t, const char *name)
{
#if SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX_ADAPTIVE
	pthread_mutexattr_t attr;

	WT_RET(pthread_mutexattr_init(&attr));
	WT_RET(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP));
	WT_RET(pthread_mutex_init(&t->lock, &attr));
#else
	WT_RET(pthread_mutex_init(&t->lock, NULL));
#endif

	t->name = name;
	t->initialized = 1;

#if SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX_LOGGING
	WT_RET(__wt_spin_lock_register_lock(session, t));
#endif

	WT_UNUSED(session);
	return (0);
}

/*
 * __wt_spin_destroy --
 *      Destroy a spinlock.
 */
static inline void
__wt_spin_destroy(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

#if SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX_LOGGING
	__wt_spin_lock_unregister_lock(session, t);
#endif
	if (t->initialized) {
		(void)pthread_mutex_destroy(&t->lock);
		t->initialized = 0;
	}
}

#if SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX ||\
	SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX_ADAPTIVE

#define	WT_DECL_SPINLOCK_ID(i)
#define	__wt_spin_trylock(session, lock, idp)				\
	__wt_spin_trylock_func(session, lock)

/*
 * __wt_spin_trylock_func --
 *      Try to lock a spinlock or fail immediately if it is busy.
 */
static inline int
__wt_spin_trylock_func(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	return (pthread_mutex_trylock(&t->lock));
}

/*
 * __wt_spin_lock --
 *      Spin until the lock is acquired.
 */
static inline void
__wt_spin_lock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	(void)pthread_mutex_lock(&t->lock);
}
#endif

#if SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX_LOGGING

/*
 * When logging statistics, we track which spinlocks block and why.
 */
#define	WT_DECL_SPINLOCK_ID(i)						\
	static int i = WT_SPINLOCK_REGISTER
#define	WT_SPINLOCK_REGISTER		-1
#define	WT_SPINLOCK_REGISTER_FAILED	-2
#define	__wt_spin_trylock(session, lock, idp)				\
	__wt_spin_trylock_func(session, lock, idp, __FILE__, __LINE__)
#define	__wt_spin_lock(session, lock) do {				\
	WT_DECL_SPINLOCK_ID(__id);					\
	__wt_spin_lock_func(session, lock, &__id, __FILE__, __LINE__);	\
} while (0)

/*
 * __wt_spin_trylock_func --
 *      Try to lock a spinlock or fail immediately if it is busy.
 */
static inline int
__wt_spin_trylock_func(WT_SESSION_IMPL *session,
    WT_SPINLOCK *t, int *idp, const char *file, int line)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	conn = S2C_SAFE(session);
	/* If we're not maintaining statistics, it's simple. */
	if (session == NULL || !FLD_ISSET(conn->stat_flags, WT_STAT_CONN_FAST))
		return (pthread_mutex_trylock(&t->lock));

	/*
	 * If this caller hasn't yet registered, do so.  The caller's location
	 * ID is a static offset into a per-connection structure, and that has
	 * problems: first, if there are multiple connections, we'll need to
	 * hold some kind of lock to avoid racing when setting that value, and
	 * second, if/when there are multiple connections and/or a single
	 * connection is closed and re-opened, the variable may be initialized
	 * and the underlying connection information may not.  Check both.
	 */
	if (*idp == WT_SPINLOCK_REGISTER ||
	    conn->spinlock_block[*idp].name == NULL)
		WT_RET(__wt_spin_lock_register_caller(
		    session, t->name, file, line, idp));

	/*
	 * Try to acquire the mutex: on failure, update blocking statistics, on
	 * success, set our ID as the mutex holder.
	 *
	 * Note the race between acquiring the lock and setting our ID as the
	 * holder, this can appear in the output as mutexes blocking in ways
	 * that can't actually happen (although still an indicator of a mutex
	 * that's busier than we'd like).
	 */
	if ((ret = pthread_mutex_trylock(&t->lock)) == 0)
		t->id = *idp;
	else
		if (*idp >= 0) {
			++conn->spinlock_block[*idp].total;
			if (t->id >= 0)
				++conn->spinlock_block[*idp].blocked[t->id];
		}

	/* Update the mutex counter and flush to minimize the windows. */
	++t->counter;
	WT_FULL_BARRIER();
	return (ret);
}

/*
 * __wt_spin_lock_func --
 *      Spin until the lock is acquired.
 */
static inline void
__wt_spin_lock_func(WT_SESSION_IMPL *session,
    WT_SPINLOCK *t, int *idp, const char *file, int line)
{
	/* If we're not maintaining statistics, it's simple. */
	if (session == NULL ||
	    !FLD_ISSET(conn->stat_flags, WT_STAT_CONN_FAST)) {
		pthread_mutex_lock(&t->lock);
		return;
	}

	/* Try to acquire the mutex. */
	if (__wt_spin_trylock_func(session, t, idp, file, line) == 0)
		return;

	/*
	 * On failure, wait on the mutex; once acquired, set our ID as the
	 * holder and flush to minimize the windows.
	 */
	pthread_mutex_lock(&t->lock);
	t->id = *idp;
	WT_FULL_BARRIER();
}

#endif

/*
 * __wt_spin_unlock --
 *      Release the spinlock.
 */
static inline void
__wt_spin_unlock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	(void)pthread_mutex_unlock(&t->lock);
}

#elif SPINLOCK_TYPE == SPINLOCK_MSVC

#define	WT_DECL_SPINLOCK_ID(i)	
#define	WT_SPINLOCK_REGISTER		-1
#define	WT_SPINLOCK_REGISTER_FAILED	-2

#define	__wt_spin_trylock(session, lock, idp)				\
	__wt_spin_trylock_func(session, lock)

/*
 * __wt_spin_init --
 *      Initialize a spinlock.
 */
static inline int
__wt_spin_init(WT_SESSION_IMPL *session, WT_SPINLOCK *t, const char *name)
{
	WT_UNUSED(session);
	WT_UNUSED(name);

	InitializeCriticalSectionAndSpinCount(&t->lock, 4000);

	return (0);
}

/*
 * __wt_spin_destroy --
 *      Destroy a spinlock.
 */
static inline void
__wt_spin_destroy(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	DeleteCriticalSection(&t->lock);
}

/*
 * __wt_spin_trylock_func --
 *      Try to lock a spinlock or fail immediately if it is busy.
 */
static inline int
__wt_spin_trylock_func(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	BOOL b = TryEnterCriticalSection(&t->lock);
	return (b == 0 ? EBUSY : 0);
}

/*
 * __wt_spin_lock --
 *      Spin until the lock is acquired.
 */
static inline void
__wt_spin_lock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	EnterCriticalSection(&t->lock);
}

/*
 * __wt_spin_unlock --
 *      Release the spinlock.
 */
static inline void
__wt_spin_unlock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	LeaveCriticalSection(&t->lock);
}

#else

#error Unknown spinlock type

#endif
