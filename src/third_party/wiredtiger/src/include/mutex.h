/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Condition variables:
 *
 * WiredTiger uses condition variables to signal between threads, and for
 * locking operations that are expected to block.
 */
struct __wt_condvar {
	const char *name;		/* Mutex name for debugging */

	wt_mutex_t mtx;			/* Mutex */
	wt_cond_t  cond;		/* Condition variable */

	int waiters;			/* Numbers of waiters, or
					   -1 if signalled with no waiters. */
	/*
	 * The following fields are only used for automatically adjusting
	 * condition variables. They could be in a separate structure.
	 */
	uint64_t	min_wait;	/* Minimum wait duration */
	uint64_t	max_wait;	/* Maximum wait duration */
	uint64_t	prev_wait;	/* Wait duration used last time */
};

/*
 * !!!
 * Don't modify this structure without understanding the read/write locking
 * functions.
 */
typedef union {				/* Read/write lock */
	uint64_t u;
	struct {
		uint32_t wr;		/* Writers and readers */
	} i;
	struct {
		uint16_t writers;	/* Now serving for writers */
		uint16_t readers;	/* Now serving for readers */
		uint16_t users;		/* Next available ticket number */
		uint16_t __notused;	/* Padding */
	} s;
} wt_rwlock_t;

/*
 * Read/write locks:
 *
 * WiredTiger uses read/write locks for shared/exclusive access to resources.
 */
struct __wt_rwlock {
	const char *name;		/* Lock name for debugging */

	wt_rwlock_t rwlock;		/* Read/write lock */
};

/*
 * A light weight lock that can be used to replace spinlocks if fairness is
 * necessary. Implements a ticket-based back off spin lock.
 * The fields are available as a union to allow for atomically setting
 * the state of the entire lock.
 */
struct __wt_fair_lock {
	union {
		uint32_t lock;
		struct {
			uint16_t owner;		/* Ticket for current owner */
			uint16_t waiter;	/* Last allocated ticket */
		} s;
	} u;
#define	fair_lock_owner u.s.owner
#define	fair_lock_waiter u.s.waiter
};

/*
 * Spin locks:
 *
 * WiredTiger uses spinlocks for fast mutual exclusion (where operations done
 * while holding the spin lock are expected to complete in a small number of
 * instructions).
 */
#define	SPINLOCK_GCC			0
#define	SPINLOCK_MSVC			1
#define	SPINLOCK_PTHREAD_MUTEX		2
#define	SPINLOCK_PTHREAD_MUTEX_ADAPTIVE	3

#if SPINLOCK_TYPE == SPINLOCK_GCC

struct WT_COMPILER_TYPE_ALIGN(WT_CACHE_LINE_ALIGNMENT) __wt_spinlock {
	volatile int lock;
};

#elif SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX ||\
	SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX_ADAPTIVE ||\
	SPINLOCK_TYPE == SPINLOCK_MSVC

struct WT_COMPILER_TYPE_ALIGN(WT_CACHE_LINE_ALIGNMENT) __wt_spinlock {
	wt_mutex_t lock;

	const char *name;		/* Statistics: mutex name */

	int8_t initialized;		/* Lock initialized, for cleanup */
};

#else

#error Unknown spinlock type

#endif
