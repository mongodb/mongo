/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
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

/* Default to spinning 1000 times before yielding. */
#ifndef WT_SPIN_COUNT
#define	WT_SPIN_COUNT 1000
#endif

static inline int
__wt_spin_init(WT_SESSION_IMPL *session, WT_SPINLOCK *t, const char *name)
{
	WT_UNUSED(session);
	WT_UNUSED(name);

	*(t) = 0;
	return (0);
}

static inline void
__wt_spin_destroy(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	*(t) = 0;
}

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

static inline int
__wt_spin_trylock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	return (__sync_lock_test_and_set(t, 1) == 0 ? 0 : EBUSY);
}

static inline void
__wt_spin_unlock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	__sync_lock_release(t);
}

#elif SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX ||\
	SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX_LOGGING

static inline int
__wt_spin_init(WT_SESSION_IMPL *session, WT_SPINLOCK *t, const char *name)
{
#ifdef HAVE_MUTEX_ADAPTIVE
	pthread_mutexattr_t attr;

	WT_RET(pthread_mutexattr_init(&attr));
	WT_RET(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP));
	WT_RET(pthread_mutex_init(&t->lock, &attr));
#else
	WT_RET(pthread_mutex_init(&t->lock, NULL));
#endif

	t->name = name;
	t->initialized = 1;

	WT_UNUSED(session);
	return (0);
}

static inline void
__wt_spin_destroy(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	if (t->initialized) {
		(void)pthread_mutex_destroy(&t->lock);
		t->initialized = 0;
	}
}

#if SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX

static inline void
__wt_spin_lock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	pthread_mutex_lock(&t->lock);
}

#endif

#if SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX_LOGGING

/*
 * When logging statistics, we track which spinlocks block and why.
 */
#define	WT_SPINLOCK_REGISTER		-1
#define	WT_SPINLOCK_REGISTER_FAILED	-2
#define	__wt_spin_lock(session, addr) do {				\
	static int __id = WT_SPINLOCK_REGISTER;				\
	__wt_spin_lock_func(session, addr, &__id, __FILE__, __LINE__);	\
} while (0)

static inline void
__wt_spin_lock_func(WT_SESSION_IMPL *session,
    WT_SPINLOCK *t, int *idp, const char *file, int line)
{
	/* If we're not maintaining statistics, it's simple. */
	if (session == NULL || !S2C(session)->stat_fast) {
		pthread_mutex_lock(&t->lock);
		return;
	}

	/* If this caller hasn't yet registered, do so. */
	if (*idp == WT_SPINLOCK_REGISTER)
		__wt_spin_lock_register(session, t, file, line, idp);

	/*
	 * Try to acquire the mutex.  On success, set our ID as the mutex holder
	 * and flush (using a full barrier to minimize the window).  On failure,
	 * update the blocking statistics and block on the mutex.
	 *
	 * Note the race between acquiring the lock and setting our ID as the
	 * holder, this can appear in the output as mutexes blocking in ways
	 * that can't actually happen (although still an indicator of a mutex
	 * that's busier than we'd like).
	 */
	if (pthread_mutex_trylock(&t->lock)) {
		if (*idp >= 0 && t->id >= 0)
			++S2C(session)->spinlock_stats[*idp].blocked[t->id];
		pthread_mutex_lock(&t->lock);
	}

	++t->counter;
	t->id = *idp;
	WT_FULL_BARRIER();
}

#endif

static inline int
__wt_spin_trylock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	return (pthread_mutex_trylock(&t->lock));
}

static inline void
__wt_spin_unlock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	pthread_mutex_unlock(&t->lock);
}

#else

#error Unknown spinlock type

#endif
