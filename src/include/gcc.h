/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define	WT_SIZET_FMT	"zu"			/* size_t format string */

/* Add GCC-specific attributes to types and function declarations. */
#define	WT_COMPILER_TYPE_ALIGN(x)	__attribute__((aligned(x)))

#define	WT_PACKED_STRUCT_BEGIN(name)					\
	struct __attribute__ ((__packed__)) name {
#define	WT_PACKED_STRUCT_END						\
	};

/*
 * Attribute are only permitted on function declarations, not definitions.
 * This macro is a marker for function definitions that is rewritten by
 * dist/s_prototypes to create extern.h.
 */
#define	WT_GCC_FUNC_ATTRIBUTE(x)
#define	WT_GCC_FUNC_DECL_ATTRIBUTE(x) __attribute__(x)

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
 * WiredTiger additionally requires atomic writes for 64-bit memory locations,
 * and so cannot run on machines with a 32-bit memory bus.
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
#define	__WT_ATOMIC_ADD(v, val, n)					\
	(WT_STATIC_ASSERT(sizeof(v) == (n)), __sync_add_and_fetch(&(v), val))
#define	__WT_ATOMIC_FETCH_ADD(v, val, n)				\
	(WT_STATIC_ASSERT(sizeof(v) == (n)), __sync_fetch_and_add(&(v), val))
#ifdef __clang__
/*
 * We avoid __sync_bool_compare_and_swap with due to problems with
 * optimization with some versions of clang.  See
 * http://llvm.org/bugs/show_bug.cgi?id=21499 for details.
 */
#define	__WT_ATOMIC_CAS(v, old, new, n)					\
	(WT_STATIC_ASSERT(sizeof(v) == (n)),				\
	__sync_val_compare_and_swap(&(v), old, new) == (old))
#else
#define	__WT_ATOMIC_CAS(v, old, new, n)					\
	(WT_STATIC_ASSERT(sizeof(v) == (n)),				\
	__sync_bool_compare_and_swap(&(v), old, new))
#endif
#define	__WT_ATOMIC_STORE(v, val, n)					\
	(WT_STATIC_ASSERT(sizeof(v) == (n)),				\
	__sync_lock_test_and_set(&(v), val))
#define	__WT_ATOMIC_SUB(v, val, n)					\
	(WT_STATIC_ASSERT(sizeof(v) == (n)), __sync_sub_and_fetch(&(v), val))

#define	WT_ATOMIC_ADD1(v, val)		__WT_ATOMIC_ADD(v, val, 1)
#define	WT_ATOMIC_FETCH_ADD1(v, val)	__WT_ATOMIC_FETCH_ADD(v, val, 1)
#define	WT_ATOMIC_CAS1(v, old, new)	__WT_ATOMIC_CAS(v, old, new, 1)
#define	WT_ATOMIC_STORE1(v, val)	__WT_ATOMIC_STORE(v, val, 1)
#define	WT_ATOMIC_SUB1(v, val)		__WT_ATOMIC_SUB(v, val, 1)

#define	WT_ATOMIC_ADD2(v, val)		__WT_ATOMIC_ADD(v, val, 2)
#define	WT_ATOMIC_FETCH_ADD2(v, val)	__WT_ATOMIC_FETCH_ADD(v, val, 2)
#define	WT_ATOMIC_CAS2(v, old, new)	__WT_ATOMIC_CAS(v, old, new, 2)
#define	WT_ATOMIC_STORE2(v, val)	__WT_ATOMIC_STORE(v, val, 2)
#define	WT_ATOMIC_SUB2(v, val)		__WT_ATOMIC_SUB(v, val, 2)

#define	WT_ATOMIC_ADD4(v, val)		__WT_ATOMIC_ADD(v, val, 4)
#define	WT_ATOMIC_FETCH_ADD4(v, val)	__WT_ATOMIC_FETCH_ADD(v, val, 4)
#define	WT_ATOMIC_CAS4(v, old, new)	__WT_ATOMIC_CAS(v, old, new, 4)
#define	WT_ATOMIC_STORE4(v, val)	__WT_ATOMIC_STORE(v, val, 4)
#define	WT_ATOMIC_SUB4(v, val)		__WT_ATOMIC_SUB(v, val, 4)

#define	WT_ATOMIC_ADD8(v, val)		__WT_ATOMIC_ADD(v, val, 8)
#define	WT_ATOMIC_FETCH_ADD8(v, val)	__WT_ATOMIC_FETCH_ADD(v, val, 8)
#define	WT_ATOMIC_CAS8(v, old, new)	__WT_ATOMIC_CAS(v, old, new, 8)
#define	WT_ATOMIC_STORE8(v, val)	__WT_ATOMIC_STORE(v, val, 8)
#define	WT_ATOMIC_SUB8(v, val)		__WT_ATOMIC_SUB(v, val, 8)

/* Compile read-write barrier */
#define	WT_BARRIER() __asm__ volatile("" ::: "memory")

/* Pause instruction to prevent excess processor bus usage */
#define	WT_PAUSE() __asm__ volatile("pause\n" ::: "memory")

#if defined(x86_64) || defined(__x86_64__)
#define	WT_FULL_BARRIER() do {						\
	__asm__ volatile ("mfence" ::: "memory");			\
} while (0)
#define	WT_READ_BARRIER() do {						\
	__asm__ volatile ("lfence" ::: "memory");			\
} while (0)
#define	WT_WRITE_BARRIER() do {						\
	__asm__ volatile ("sfence" ::: "memory");			\
} while (0)

#elif defined(i386) || defined(__i386__)
#define	WT_FULL_BARRIER() do {						\
	__asm__ volatile ("lock; addl $0, 0(%%esp)" ::: "memory");	\
} while (0)
#define	WT_READ_BARRIER()	WT_FULL_BARRIER()
#define	WT_WRITE_BARRIER()	WT_FULL_BARRIER()

#else
#error "No write barrier implementation for this hardware"
#endif
