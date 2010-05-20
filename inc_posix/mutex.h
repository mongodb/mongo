/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2009 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

/*
 * Atomic writes:
 *
 * WiredTiger requires variables of type wt_atomic_t and pointers (void *), be
 * written atomically, in a single cycle.
 *
 * None of the wt_atomic_t values are counters, that is, they are boolean types,
 * valued either 0 or 1.  To keep in-memory structures smaller, we use a 32-bit
 * type on 64-bit machines, which is OK if the compiler doesn't accumulate two
 * 32-bit chunks into a single 64-bit write, that is, there needs to be a single
 * load/store of 32-bits, not a load/store of 64-bits, where the 64-bits is
 * comprised of two adjacent 32-bit memory locations.  If that can happen, you
 * must increase the size of the wt_atomic_t type to a type guaranteed to be
 * written atomically in a single cycle.
 *
 * WiredTiger doesn't require write ordering, only that when a wt_atomic_t type
 * or pointer is simultaneously written by two threads of control, the result is
 * one or the other of the two values, not some combination of both.
 *
 * WiredTiger doesn't require atomic writes for any 64-bit memory locations and
 * can run on machines with a 32-bit memory bus.
 */
typedef u_int32_t wt_atomic_t;

/*
 * Atomic writes are often associated with memory barriers, implemented by the
 * WT_MEMORY_FLUSH macro.   The WT_MEMORY_FLUSH macro ensures memory stores by
 * the processor, made before the WT_MEMORY_FLUSH call, be visible to all
 * processors in the system, before any memory stores by this processor, made
 * after the WT_MEMORY_FLUSH call, are visible to any processor.  Note that the
 * WT_MEMORY_FLUSH macro makes no requirement with respect to loads by any
 * processor, so any memory reference which might cause a problem if cached by
 * a processor needs to be declared volatile.
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
