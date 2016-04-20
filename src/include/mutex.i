/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
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

/* Default to spinning 1000 times before yielding. */
#ifndef WT_SPIN_COUNT
#define	WT_SPIN_COUNT WT_THOUSAND
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

	t->lock = 0;
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

	t->lock = 0;
}

/*
 * __wt_spin_trylock --
 *      Try to lock a spinlock or fail immediately if it is busy.
 */
static inline int
__wt_spin_trylock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	return (__sync_lock_test_and_set(&t->lock, 1) == 0 ? 0 : EBUSY);
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

	while (__sync_lock_test_and_set(&t->lock, 1)) {
		for (i = 0; t->lock && i < WT_SPIN_COUNT; i++)
			WT_PAUSE();
		if (t->lock)
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

	__sync_lock_release(&t->lock);
}

#elif SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX ||\
	SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX_ADAPTIVE

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

	if (t->initialized) {
		(void)pthread_mutex_destroy(&t->lock);
		t->initialized = 0;
	}
}

#if SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX ||\
	SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX_ADAPTIVE

/*
 * __wt_spin_trylock --
 *      Try to lock a spinlock or fail immediately if it is busy.
 */
static inline int
__wt_spin_trylock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
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

#define	WT_SPINLOCK_REGISTER		-1
#define	WT_SPINLOCK_REGISTER_FAILED	-2

/*
 * __wt_spin_init --
 *      Initialize a spinlock.
 */
static inline int
__wt_spin_init(WT_SESSION_IMPL *session, WT_SPINLOCK *t, const char *name)
{
	WT_UNUSED(session);

	t->name = name;
	t->initialized = 1;

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

	if (t->initialized) {
		DeleteCriticalSection(&t->lock);
		t->initialized = 0;
	}
}

/*
 * __wt_spin_trylock --
 *      Try to lock a spinlock or fail immediately if it is busy.
 */
static inline int
__wt_spin_trylock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
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

/*
 * __wt_fair_trylock --
 *	Try to get a lock - give up if it is not immediately available.
 */
static inline int
__wt_fair_trylock(WT_SESSION_IMPL *session, WT_FAIR_LOCK *lock)
{
	WT_FAIR_LOCK new, old;

	WT_UNUSED(session);

	old = new = *lock;

	/* Exit early if there is no chance we can get the lock. */
	if (old.fair_lock_waiter != old.fair_lock_owner)
		return (EBUSY);

	/* The replacement lock value is a result of allocating a new ticket. */
	++new.fair_lock_waiter;
	return (__wt_atomic_cas32(
	    &lock->u.lock, old.u.lock, new.u.lock) ? 0 : EBUSY);
}

/*
 * __wt_fair_lock --
 *	Get a lock.
 */
static inline int
__wt_fair_lock(WT_SESSION_IMPL *session, WT_FAIR_LOCK *lock)
{
	uint16_t ticket;
	int pause_cnt;

	WT_UNUSED(session);

	/*
	 * Possibly wrap: if we have more than 64K lockers waiting, the ticket
	 * value will wrap and two lockers will simultaneously be granted the
	 * lock.
	 */
	ticket = __wt_atomic_fetch_add16(&lock->fair_lock_waiter, 1);
	for (pause_cnt = 0; ticket != lock->fair_lock_owner;) {
		/*
		 * We failed to get the lock; pause before retrying and if we've
		 * paused enough, sleep so we don't burn CPU to no purpose. This
		 * situation happens if there are more threads than cores in the
		 * system and we're thrashing on shared resources.
		 */
		if (++pause_cnt < WT_THOUSAND)
			WT_PAUSE();
		else
			__wt_sleep(0, 10);
	}

	/*
	 * Applications depend on a barrier here so that operations holding the
	 * lock see consistent data.
	 */
	WT_READ_BARRIER();

	return (0);
}

/*
 * __wt_fair_unlock --
 *	Release a shared lock.
 */
static inline int
__wt_fair_unlock(WT_SESSION_IMPL *session, WT_FAIR_LOCK *lock)
{
	WT_UNUSED(session);

	/*
	 * Ensure that all updates made while the lock was held are visible to
	 * the next thread to acquire the lock.
	 */
	WT_WRITE_BARRIER();

	/*
	 * We have exclusive access - the update does not need to be atomic.
	 */
	++lock->fair_lock_owner;

	return (0);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_fair_islocked --
 *	Test whether the lock is currently held.
 */
static inline bool
__wt_fair_islocked(WT_SESSION_IMPL *session, WT_FAIR_LOCK *lock)
{
	WT_UNUSED(session);

	return (lock->fair_lock_waiter != lock->fair_lock_owner);
}
#endif
