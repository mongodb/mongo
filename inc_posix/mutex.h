/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2009 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

/*
 * Mutex.
 */
struct __wt_mtx {
	pthread_mutex_t mtx;		/* Mutex */			\
	pthread_cond_t  cond;		/* Condition variable */

	int locked;			/* Mutex is locked */
};

extern void *__wt_addr;

/*
 * Memory flush primitive.
 */
#if defined(sun)
#define WT_FLUSH_MEMORY							\
	membar_enter()
#elif defined(sparc) && defined(__GNUC__)
#define WT_FLUSH_MEMORY							\
	({asm volatile("stbar");})
#elif (defined(x86_64) || defined(__x86_64__)) && defined(__GNUC__)
#define	WT_FLUSH_MEMORY							\
    ({ asm volatile ("mfence" ::: "memory"); 1; })
#elif (defined(i386) || defined(__i386__)) && defined(__GNUC__)
#define	WT_FLUSH_MEMORY							\
    ({ asm volatile ("lock; addl $0, %0" ::"m" (__wt_addr): "memory"); 1; })
#endif
