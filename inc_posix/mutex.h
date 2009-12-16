/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2009 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

/*
 * There are 4 types of synchronization in WiredTiger: atomic writes, mutexes,
 * serialization, and private serialization.
 */

/*
 * Atomic writes:
 *
 * WiredTiger requires variables of type u_int32_t and pointers (void *), be
 * written atomically.  This says nothing about write ordering, only that if
 * a 32-bit memory location or a pointer is simultaneously written by two
 * threads of control, the result will be one or the other of the two values,
 * not a combination of both.  WiredTiger does not require this for 64-bit
 * memory locations, allowing it to run on 32-bit architectures.
 *
 * Atomic writes are often associated with memory flushing, implemented by
 * the WT_MEMORY_FLUSH macro.   The WT_MEMORY_FLUSH macro must flush all
 * writes by any processor in the system.
 */
extern void *__wt_addr;

#if defined(sun)
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
	pthread_mutex_t mtx;		/* Mutex */
	pthread_cond_t  cond;		/* Condition variable */

	int locked;			/* Mutex is locked */
};

/*
 * WT_TOC_GEN_IGNORE --
 *	The api_gen of a WT_TOC that's outside the library, or in the library
 *	but known to be holding no resources (used when long-running threads
 *	want to let other threads proceed).
 */
#define	WT_TOC_GEN_IGNORE	0

/*
 * WT_TOC_GEN_BLOCK --
 *	An api_gen that always blocks a running WT_TOC thread.
 */
#define	WT_TOC_GEN_BLOCK	1

/*
 * WT_TOC_GEN_MIN --
 *	The starting api_gen.
 */
#define	WT_TOC_GEN_MIN		5
