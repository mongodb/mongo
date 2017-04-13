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
	 * The following fields are used for automatically adjusting condition
	 * variable wait times.
	 */
	uint64_t	min_wait;	/* Minimum wait duration */
	uint64_t	max_wait;	/* Maximum wait duration */
	uint64_t	prev_wait;	/* Wait duration used last time */
};

/*
 * Read/write locks:
 *
 * WiredTiger uses read/write locks for shared/exclusive access to resources.
 * !!!
 * Don't modify this structure without understanding the read/write locking
 * functions.
 */
union __wt_rwlock {			/* Read/write lock */
	uint64_t u;
	struct {
		uint32_t wr;		/* Writers and readers */
	} i;
	struct {
		uint16_t writers;	/* Now serving for writers */
		uint16_t readers;	/* Now serving for readers */
		uint16_t next;		/* Next available ticket number */
		uint16_t writers_active;/* Count of active writers */
	} s;
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

struct __wt_spinlock {
	WT_CACHE_LINE_PAD_BEGIN
#if SPINLOCK_TYPE == SPINLOCK_GCC
	volatile int lock;
#elif SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX ||\
	SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX_ADAPTIVE ||\
	SPINLOCK_TYPE == SPINLOCK_MSVC
	wt_mutex_t lock;
#else
#error Unknown spinlock type
#endif

	const char *name;		/* Mutex name */

	/*
	 * We track acquisitions and time spent waiting for some locks. For
	 * performance reasons and to make it possible to write generic code
	 * that tracks statistics for different locks, we store the offset
	 * of the statistics fields to be updated during lock acquisition.
	 */
	int16_t stat_count_off;		/* acquisitions offset */
	int16_t stat_app_usecs_off;	/* waiting application threads offset */
	int16_t stat_int_usecs_off;	/* waiting server threads offset */

	int8_t initialized;		/* Lock initialized, for cleanup */
	WT_CACHE_LINE_PAD_END
};
