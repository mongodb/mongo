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
 * WiredTiger requires variables of type u_int32_t and pointers (void *), be
 * written atomically.  If that's not guaranteed for your processor, you won't
 * be able to make this software run reliably.
 *
 * This does not require write ordering, only that if a 32-bit memory location
 * or a pointer is simultaneously written by two threads of control, the result
 * will be one or the other of the two values, not a combination of both.  This
 * softare does NOT require atomic writes for 64-bit memory locations, allowing
 * it to run on 32-bit memory bus architectures.
 *
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
