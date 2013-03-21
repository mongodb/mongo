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
__wt_spin_init(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	*(t) = 0;
}

static inline void
__wt_spin_destroy(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);
	WT_UNUSED(t);
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
__wt_spin_init(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
#ifdef HAVE_MUTEX_ADAPTIVE
	pthread_mutexattr_t attr;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP);
	(void)pthread_mutex_init(t, &attr);
#else
	(void)pthread_mutex_init(t, NULL);
#endif

	WT_UNUSED(session);
}

static inline void
__wt_spin_destroy(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	(void)pthread_mutex_destroy(t);
}

static inline void
__wt_spin_lock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);
	pthread_mutex_lock(t);
}

static inline int
__wt_spin_trylock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);
	return (pthread_mutex_trylock(t));
}

static inline void
__wt_spin_unlock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);
	pthread_mutex_unlock(t);
}

#else

#error Unknown spinlock type

#endif
