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
 * 24-bits were written, WiredTiger would break. Or, if two threads of control
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
 * memory location to 0 and flushes). If thread X were to read the 32 bits
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
 * atomic state changes, we rely on the atomic-add and atomic-cas (compare and
 * swap) operations.
 */

static inline uint8_t
 __wt_atomic_add1(uint8_t *vp, uint8_t v)
{
	return (__sync_add_and_fetch(vp, v));
}
static inline uint8_t
 __wt_atomic_fetch_add1(uint8_t *vp, uint8_t v)
{
	return (__sync_fetch_and_add(vp, v));
}
static inline uint8_t
 __wt_atomic_store1(uint8_t *vp, uint8_t v)
{
	return (__sync_lock_test_and_set(vp, v));
}
static inline uint8_t
 __wt_atomic_sub1(uint8_t *vp, uint8_t v)
{
	return (__sync_sub_and_fetch(vp, v));
}
static inline int
 __wt_atomic_cas1(uint8_t *vp, uint8_t old, uint8_t new)
{
#ifdef __clang__
	/*
	 * We avoid __sync_bool_compare_and_swap with due to problems with
	 * optimization with some versions of clang.
	 * See http://llvm.org/bugs/show_bug.cgi?id=21499 for details.
	 */
	return (__sync_val_compare_and_swap(vp, old, new) == old);
#else
	return (__sync_bool_compare_and_swap(vp, old, new);
#endif
}

static inline uint16_t
 __wt_atomic_add2(uint16_t *vp, uint16_t v)
{
	return (__sync_add_and_fetch(vp, v));
}
static inline uint16_t
 __wt_atomic_fetch_add2(uint16_t *vp, uint16_t v)
{
	return (__sync_fetch_and_add(vp, v));
}
static inline uint16_t
 __wt_atomic_store2(uint16_t *vp, uint16_t v)
{
	return (__sync_lock_test_and_set(vp, v));
}
static inline uint16_t
 __wt_atomic_sub2(uint16_t *vp, uint16_t v)
{
	return (__sync_sub_and_fetch(vp, v));
}
static inline int
 __wt_atomic_cas2(uint16_t *vp, uint16_t old, uint16_t new)
{
#ifdef __clang__
	/*
	 * We avoid __sync_bool_compare_and_swap with due to problems with
	 * optimization with some versions of clang.
	 * See http://llvm.org/bugs/show_bug.cgi?id=21499 for details.
	 */
	return (__sync_val_compare_and_swap(vp, old, new) == old);
#else
	return (__sync_bool_compare_and_swap(vp, old, new);
#endif
}

static inline int32_t
 __wt_atomic_addi4(int32_t *vp, int32_t v)
{
	return (__sync_add_and_fetch(vp, v));
}
static inline int32_t
 __wt_atomic_fetch_addi4(int32_t *vp, int32_t v)
{
	return (__sync_fetch_and_add(vp, v));
}
static inline int32_t
 __wt_atomic_storei4(int32_t *vp, int32_t v)
{
	return (__sync_lock_test_and_set(vp, v));
}
static inline int32_t
 __wt_atomic_subi4(int32_t *vp, int32_t v)
{
	return (__sync_sub_and_fetch(vp, v));
}
static inline int
 __wt_atomic_casi4(int32_t *vp, int32_t old, int32_t new)
{
#ifdef __clang__
	/*
	 * We avoid __sync_bool_compare_and_swap with clang due to problems with
	 * optimization in some versions of clang.
	 * See http://llvm.org/bugs/show_bug.cgi?id=21499 for details.
	 */
	return (__sync_val_compare_and_swap(vp, old, new) == old);
#else
	return (__sync_bool_compare_and_swap(vp, old, new);
#endif
}

static inline uint32_t
 __wt_atomic_add4(uint32_t *vp, uint32_t v)
{
	return (__sync_add_and_fetch(vp, v));
}
static inline uint32_t
 __wt_atomic_fetch_add4(uint32_t *vp, uint32_t v)
{
	return (__sync_fetch_and_add(vp, v));
}
static inline uint32_t
 __wt_atomic_store4(uint32_t *vp, uint32_t v)
{
	return (__sync_lock_test_and_set(vp, v));
}
static inline uint32_t
 __wt_atomic_sub4(uint32_t *vp, uint32_t v)
{
	return (__sync_sub_and_fetch(vp, v));
}
static inline int
 __wt_atomic_cas4(uint32_t *vp, uint32_t old, uint32_t new)
{
#ifdef __clang__
	/*
	 * We avoid __sync_bool_compare_and_swap with clang due to problems with
	 * optimization in some versions of clang.
	 * See http://llvm.org/bugs/show_bug.cgi?id=21499 for details.
	 */
	return (__sync_val_compare_and_swap(vp, old, new) == old);
#else
	return (__sync_bool_compare_and_swap(vp, old, new);
#endif
}

static inline int64_t
 __wt_atomic_addi8(int64_t *vp, int64_t v)
{
	return (__sync_add_and_fetch(vp, v));
}
static inline int64_t
 __wt_atomic_fetch_addi8(int64_t *vp, int64_t v)
{
	return (__sync_fetch_and_add(vp, v));
}
static inline int64_t
 __wt_atomic_storei8(int64_t *vp, int64_t v)
{
	return (__sync_lock_test_and_set(vp, v));
}
static inline int64_t
 __wt_atomic_subi8(int64_t *vp, int64_t v)
{
	return (__sync_sub_and_fetch(vp, v));
}
static inline int
 __wt_atomic_casi8(int64_t *vp, int64_t old, int64_t new)
{
#ifdef __clang__
	/*
	 * We avoid __sync_bool_compare_and_swap with due to problems with
	 * optimization with some versions of clang.
	 * See http://llvm.org/bugs/show_bug.cgi?id=21499 for details.
	 */
	return (__sync_val_compare_and_swap(vp, old, new) == old);
#else
	return (__sync_bool_compare_and_swap(vp, old, new);
#endif
}

static inline uint64_t
 __wt_atomic_add8(uint64_t *vp, uint64_t v)
{
	return (__sync_add_and_fetch(vp, v));
}
static inline uint64_t
 __wt_atomic_fetch_add8(uint64_t *vp, uint64_t v)
{
	return (__sync_fetch_and_add(vp, v));
}
static inline uint64_t
 __wt_atomic_store8(uint64_t *vp, uint64_t v)
{
	return (__sync_lock_test_and_set(vp, v));
}
static inline uint64_t
 __wt_atomic_sub8(uint64_t *vp, uint64_t v)
{
	return (__sync_sub_and_fetch(vp, v));
}
static inline int
 __wt_atomic_cas8(uint64_t *vp, uint64_t old, uint64_t new)
{
#ifdef __clang__
	/*
	 * We avoid __sync_bool_compare_and_swap with due to problems with
	 * optimization with some versions of clang.
	 * See http://llvm.org/bugs/show_bug.cgi?id=21499 for details.
	 */
	return (__sync_val_compare_and_swap(vp, old, new) == old);
#else
	return (__sync_bool_compare_and_swap(vp, old, new);
#endif
}
static inline int
 __wt_atomic_cas_ptr(void *vp, void *old, void *new)
{
#ifdef __clang__
	/*
	 * We avoid __sync_bool_compare_and_swap with due to problems with
	 * optimization with some versions of clang.
	 * See http://llvm.org/bugs/show_bug.cgi?id=21499 for details.
	 */
	return (__sync_val_compare_and_swap((void **)vp, old, new) == old);
#else
	return (__sync_bool_compare_and_swap((void **)vp, old, new);
#endif
}

/* Compile read-write barrier */
#define	WT_BARRIER() __asm__ volatile("" ::: "memory")

#if defined(x86_64) || defined(__x86_64__)
/* Pause instruction to prevent excess processor bus usage */
#define	WT_PAUSE() __asm__ volatile("pause\n" ::: "memory")

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
#define	WT_PAUSE() __asm__ volatile("pause\n" ::: "memory")
#define	WT_FULL_BARRIER() do {						\
	__asm__ volatile ("lock; addl $0, 0(%%esp)" ::: "memory");	\
} while (0)
#define	WT_READ_BARRIER()	WT_FULL_BARRIER()
#define	WT_WRITE_BARRIER()	WT_FULL_BARRIER()

#elif defined(__PPC64__) || defined(PPC64)
#define	WT_PAUSE()	__asm__ volatile("ori 0,0,0" ::: "memory")
#define	WT_FULL_BARRIER()	do {
	__asm__ volatile ("sync" ::: "memory");				\
} while (0)
#define	WT_READ_BARRIER()	WT_FULL_BARRIER()
#define	WT_WRITE_BARRIER()	WT_FULL_BARRIER()

#elif defined(__aarch64__)
#define	WT_PAUSE()	__asm__ volatile("yield" ::: "memory")
#define	WT_FULL_BARRIER() do {						\
	  __asm__ volatile ("dsb sy" ::: "memory");			\
} while (0)
#define	WT_READ_BARRIER() do {						\
	  __asm__ volatile ("dsb ld" ::: "memory");			\
} while (0)
#define	WT_WRITE_BARRIER() do {						\
	  __asm__ volatile ("dsb st" ::: "memory");			\
} while (0)

#else
#error "No write barrier implementation for this hardware"
#endif
