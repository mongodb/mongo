/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
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
 * 24-bits were written, WiredTiger would * break.   Or, if two threads of
 * control attempt to write the same location simultaneously, the result must be
 * one or the other of the two values, not some combination of both.
 *
 * To reduce memory requirements, we use a 32-bit type on 64-bit machines, which
 * is OK if the compiler doesn't accumulate two adjacent 32-bit variables into a
 * single 64-bit write, that is, there needs to be a single load/store of the 32
 * bits, not a load/store of 64 bits, where the 64 bits is comprised of two
 * adjacent 32-bit locations.  The problem is when two threads are cooperating
 * (thread X finds 32-bits set to 0, writes in a new value, flushes memory;
 * thread Y  reads 32-bits that are non-zero, does some operation, resets the
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
 * that never happens, we check address alignment in debugging mode -- we have
 * not seen any architectures with cache lines other than a multiple of 4 bytes
 * in size, so an aligned 4-byte access will always be in a single cache line.
 *
 * Atomic writes are often associated with memory barriers, implemented by the
 * WT_MEMORY_FLUSH macro.  WiredTiger's requirement as described by the Solaris
 * membar_enter description:
 *
 *	No stores from after the memory barrier will reach visibility and
 *	no loads from after the barrier will be resolved before the lock
 *	acquisition reaches global visibility
 *
 * In other words, the WT_MEMORY_FLUSH macro must ensure that memory stores by
 * the processor, made before the WT_MEMORY_FLUSH call, be visible to all
 * processors in the system before any memory stores by the processor, made
 * after the WT_MEMORY_FLUSH call, are visible to any processor.  In addition,
 * the processor will discard registers across WT_MEMORY_FLUSH calls.  The
 * WT_MEMORY_FLUSH macro makes no requirement with respect to loads by any
 * other processor than the processor making the call.
 *
 * Code handling shared data has two choices: either mark the data location
 * volatile, forcing the compiler to re-load the data on each access, or, use
 * a memory barrier instruction at appropriate points in order to force an
 * explicit re-load of the memory location.
 */
extern void *__wt_addr;

#if defined(_lint)
#define	WT_MEMORY_FLUSH
#elif defined(sun)
#include <atomic.h>
#define WT_MEMORY_FLUSH							\
	membar_enter()
#elif defined(sparc) && defined(__GNUC__)
#define WT_MEMORY_FLUSH							\
	({asm volatile("stbar");})
#elif (defined(x86_64) || defined(__x86_64__)) && defined(__GNUC__)
#define	WT_MEMORY_FLUSH							\
    ({ asm volatile ("mfence" ::: "memory"); 1; })
#elif (defined(i386) || defined(__i386__)) && defined(__GNUC__)
#define	WT_MEMORY_FLUSH							\
    ({ asm volatile ("lock; addl $0, %0" ::"m" (__wt_addr): "memory"); 1; })
#endif

/*
 * Mutexes:
 *
 * WiredTiger uses standard pthread mutexes to lock data without any particular
 * performance requirements.
 */
struct __wt_mtx {
	const char *name;		/* Mutex name for debugging */

	pthread_mutex_t mtx;		/* Mutex */
	pthread_cond_t  cond;		/* Condition variable */

	int locked;			/* Mutex is locked */
};
