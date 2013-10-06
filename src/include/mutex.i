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

static inline void
__wt_spin_lock_func(WT_SESSION_IMPL *session,
    WT_SPINLOCK *t, int *slnop, const char *file, int line
)
{
	int i;

	WT_UNUSED(session);
	WT_UNUSED(slnop);
	WT_UNUSED(file);
	WT_UNUSED(line);

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

	return (__sync_lock_test_and_set(t, 1));
}

static inline void
__wt_spin_unlock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	__sync_lock_release(t);
}

#elif SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX

static inline void
__wt_spin_lock_func(WT_SESSION_IMPL *session,
    WT_SPINLOCK *t, int *slnop, const char *file, int line)
{
	/* If we're not maintaining statistics on the spinlocks, it's simple. */
	if (session == NULL || !S2C(session)->statistics) {
		pthread_mutex_lock(&t->lock);
		return;
	}

	/* If this caller hasn't yet registered, do so. */
	if (*slnop == WT_SPINLOCK_REGISTER)
		__wt_spin_lock_register(session, file, line, t->name, slnop);

	/* Try to acquire the lock, if we fail, update blocked information. */
	if (pthread_mutex_trylock(&t->lock)) {
		/* Update blocking count. */
		if (*slnop >= 0 && t->id >= 0)
			++S2C(session)->spinlock_stats[*slnop].blocked[t->id];

		/* Block and wait. */
		pthread_mutex_lock(&t->lock);
	}

	/* We own the mutex, flush our ID as the holder. */
	t->id = *slnop;
	WT_WRITE_BARRIER();
}

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
