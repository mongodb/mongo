/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Atomic writes:
 *
 * WiredTiger requires pointers (void *) and some variables to be read/written
 * atomically, that is, in a single cycle.  This is not write ordering -- to be
 * clear, the requirement is that no partial value can ever be read or written.
 * For example, if 8-bits of a 32-bit quantity were written, then the rest of
 * the 32-bits were written, and another thread of control was able to read the
 * memory location after the first 8-bits were written and before the subsequent
 * 24-bits were written, WiredTiger would break.   Or, if two threads of control
 * attempt to write the same location simultaneously, the result must be one or
 * the other of the two values, not some combination of both.
 *
 * To reduce memory requirements, we use a 32-bit type on 64-bit machines, which
 * is OK if the compiler doesn't accumulate two adjacent 32-bit variables into a
 * single 64-bit write, that is, there needs to be a single load/store of the 32
 * bits, not a load/store of 64 bits, where the 64 bits is comprised of two
 * adjacent 32-bit locations.  The problem is when two threads are cooperating
 * (thread X finds 32-bits set to 0, writes in a new value, flushes memory;
 * thread Y reads 32-bits that are non-zero, does some operation, resets the
 * memory location to 0 and flushes).   If thread X were to read the 32 bits
 * adjacent to a different 32 bits, and write them both, the two threads could
 * race.  If that can happen, you must increase the size of the memory type to
 * a type guaranteed to be written atomically in a single cycle, without writing
 * an adjacent memory location.
 *
 * WiredTiger doesn't require atomic writes for any 64-bit memory locations and
 * can run on machines with a 32-bit memory bus.
 *
 * We don't depend on writes across cache lines being atomic, and to make sure
 * that never happens, we check address alignment: we know of no architectures
 * with cache lines other than a multiple of 4 bytes in size, so aligned 4-byte
 * accesses will always be in a single cache line.
 *
 * Atomic writes are often associated with memory barriers, implemented by the
 * WT_READ_BARRIER and WT_WRITE_BARRIER macros.  WiredTiger's requirement as
 * described by the Solaris membar_enter description:
 *
 *	No stores from after the memory barrier will reach visibility and
 *	no loads from after the barrier will be resolved before the lock
 *	acquisition reaches global visibility
 *
 * In other words, the WT_WRITE_BARRIER macro must ensure that memory stores by
 * the processor, made before the WT_WRITE_BARRIER call, be visible to all
 * processors in the system before any memory stores by the processor, made
 * after the WT_WRITE_BARRIER call, are visible to any processor.  The
 * WT_READ_BARRIER macro ensures that all loads before the barrier are complete
 * before any loads after the barrier.  The compiler cannot reorder or cache
 * values across a barrier.
 *
 * Lock and unlock operations imply both read and write barriers.  In other
 * words, barriers are not required for values protected by locking.
 *
 * Data locations may also be marked volatile, forcing the compiler to re-load
 * the data on each access.  This is a weaker semantic than barriers provide,
 * only ensuring that the compiler will not cache values.  It makes no ordering
 * guarantees and may have no effect on systems with weaker cache guarantees.
 *
 * In summary, locking > barriers > volatile.
 *
 * To avoid locking shared data structures such as statistics and to permit
 * atomic state changes, we rely on the WT_ATOMIC_ADD and WT_ATOMIC_CAS
 * (compare and swap) operations.
 */
#if defined(_lint)
#define	WT_ATOMIC_ADD(v, val)	((v) += (val), (v))
#define	WT_ATOMIC_CAS(v, oldv, newv)					\
	((v) == (oldv) && (v) = (newv) ? 1 : 0)
#define	WT_ATOMIC_CAS_VAL(v, oldv, newv)				\
	((v) == (oldv) && (v) = (newv) ? (oldv) : (v))
#define	WT_ATOMIC_STORE(v, val)	((v) = (val))
#define	WT_ATOMIC_SUB(v, val)	((v) -= (val), (v))
#define	WT_FULL_BARRIER()
#define	WT_READ_BARRIER()
#define	WT_WRITE_BARRIER()
#define	HAVE_ATOMICS 1
#elif defined(__GNUC__)
#define	WT_ATOMIC_ADD(v, val)						\
	__sync_add_and_fetch(&(v), val)
#define	WT_ATOMIC_CAS(v, oldv, newv)					\
	__sync_bool_compare_and_swap(&(v), oldv, newv)
#define	WT_ATOMIC_CAS_VAL(v, oldv, newv)				\
	__sync_val_compare_and_swap(&(v), oldv, newv)
#define	WT_ATOMIC_STORE(v, val)						\
	__sync_lock_test_and_set(&(v), val)
#define	WT_ATOMIC_SUB(v, val)						\
	__sync_sub_and_fetch(&(v), val)

#if defined(x86_64) || defined(__x86_64__)
#define	WT_FULL_BARRIER() do {						\
	asm volatile ("mfence" ::: "memory");				\
} while (0)
#define	WT_READ_BARRIER() do {						\
	asm volatile ("lfence" ::: "memory");				\
} while (0)
#define	WT_WRITE_BARRIER() do {						\
	asm volatile ("sfence" ::: "memory");				\
} while (0)
#define	HAVE_ATOMICS 1
#elif defined(i386) || defined(__i386__)
#define	WT_FULL_BARRIER() do {						\
	asm volatile ("lock; addl $0, 0(%%esp)" ::: "memory");		\
} while (0);
#define	WT_READ_BARRIER() WT_FULL_BARRIER()
#define	WT_WRITE_BARRIER() WT_FULL_BARRIER()
#define	HAVE_ATOMICS 1
#endif
#endif

#ifndef HAVE_ATOMICS
#error "No write barrier implementation for this platform"
#endif

/*
 * Publish a value to a shared location.  All previous stores must complete
 * before the value is made public.
 */
#define	WT_PUBLISH(v, val) do {						\
	WT_WRITE_BARRIER();						\
	(v) = (val);							\
} while (0)

/*
 * Read a shared location and guarantee that subsequent reads do not see any
 * earlier state.
 */
#define	WT_ORDERED_READ(v, val) do {					\
	(v) = (val);							\
	WT_READ_BARRIER();						\
} while (0)

/*
 * Atomic versions of F_ISSET, F_SET and F_CLR.
 * Spin until the new value can be swapped into place.
 */
#define	F_ISSET_ATOMIC(p, mask)	((p)->flags_atomic & (uint32_t)(mask))

#define	F_SET_ATOMIC(p, mask)	do {					\
	uint32_t __orig;						\
	do {								\
		__orig = (p)->flags_atomic;				\
	} while (!WT_ATOMIC_CAS((p)->flags_atomic,			\
	    __orig, __orig | (uint32_t)(mask)));			\
} while (0)

#define	F_CLR_ATOMIC(p, mask)	do {					\
	uint32_t __orig;						\
	do {								\
		__orig = (p)->flags_atomic;				\
	} while (!WT_ATOMIC_CAS((p)->flags_atomic,			\
	    __orig, __orig & ~(uint32_t)(mask)));			\
} while (0)

/*
 * Condition variables:
 *
 * WiredTiger uses standard pthread condition variables to signal between
 * threads, and for locking operations that are expected to block.
 */
struct __wt_condvar {
	const char *name;		/* Mutex name for debugging */

	pthread_mutex_t mtx;		/* Mutex */
	pthread_cond_t  cond;		/* Condition variable */

	int signalled;			/* Condition signalled */
};

/*
 * Read/write locks:
 *
 * WiredTiger uses standard pthread rwlocks to get shared and exclusive access
 * to resources.
 */
struct __wt_rwlock {
	const char *name;		/* Lock name for debugging */

	pthread_rwlock_t rwlock;	/* Read/write lock */
};

/* Compile read-write barrier */
#define	WT_BARRIER() asm volatile("" ::: "memory")

/* Pause instruction to prevent excess processor bus usage */
#define	WT_PAUSE() asm volatile("pause\n" ::: "memory")

/*
 * Spin locks:
 *
 * These used for cases where fast mutual exclusion is needed (where operations
 * done while holding the spin lock are expected to complete in a small number
 * of instructions.
 */
#define	SPINLOCK_GCC 0
#define	SPINLOCK_PTHREAD_MUTEX 1

#if SPINLOCK_TYPE == SPINLOCK_GCC

typedef	volatile int WT_SPINLOCK;

#elif SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX

typedef pthread_mutex_t WT_SPINLOCK;

#else

#error Unknown spinlock type

#endif
