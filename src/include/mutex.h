/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
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
};

/*
 * !!!
 * Don't touch this structure without understanding the read/write
 * locking functions.
 */
typedef union {			/* Read/write lock */
#ifdef WORDS_BIGENDIAN
	WiredTiger read/write locks require modification for big-endian systems.
#else
	uint64_t u;
	struct {
		uint32_t us;
	} i;
	struct {
		uint16_t writers;
		uint16_t readers;
		uint16_t users;
		uint16_t pad;
	} s;
#endif
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

typedef volatile int WT_COMPILER_TYPE_ALIGN(WT_CACHE_LINE_ALIGNMENT)
    WT_SPINLOCK;

#elif SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX ||\
	SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX_ADAPTIVE ||\
	SPINLOCK_TYPE == SPINLOCK_MSVC

typedef WT_COMPILER_TYPE_ALIGN(WT_CACHE_LINE_ALIGNMENT) struct {
	wt_mutex_t lock;

	const char *name;		/* Statistics: mutex name */

	int8_t initialized;		/* Lock initialized, for cleanup */
} WT_SPINLOCK;

#else

#error Unknown spinlock type

#endif
