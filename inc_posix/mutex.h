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
 *
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
 * Serialization:
 *
 * Serialization support allows scheduling operations that require serialized
 * access to data structures, but which do not require readers to block from
 * using those data structures.  For example, updating the bytes allocated in
 * the cache does not require readers to quit reading, only that writers update
 * serially.
 *
 * Threads serializing set a function pointer/argument pair in their WT_TOC
 * handles, then flush memory and spin; the workQ thread notes the set and
 * calls the function.  When the function returns, the workQ thread clears
 * the WT_TOC field and the original thread can proceed.  Scheduling threads
 * usually divide work into two parts: work done before serialization is
 * required, and work requiring serialization.
 *
 * Private serialization:
 *
 * Private serialization support allows threads of control to block both
 * readers and writers from a data structure while an operation is done.
 * For example, removing pages from the cache requires that no threads be
 * using the page when it's discarded.
 *
 * Threads needing private serialization set the function pointer/argument
 * described for serialization, an additionally set a pointer to a memory
 * location that acts as a serialization point.  The workQ thread sets this
 * memory location to an API generation number.   Threads with API generation
 * numbers equal-to-or-lower than the serialization point's value continue
 * through the serialization point.  Threads with API generation numbers
 * greater than the set value pause.  When the workQ determines no thread
 * in the system has an API generation number lower than the serialization
 * point's, it calls the serialization function.  When that function returns,
 * the serialization point is cleared, and all waiters proceed.
 *
 * WT_TOC_SERIALIZE_REQUEST --
 *	Configure a serialization request.
 */
#define	WT_TOC_SERIALIZE_REQUEST(toc, f, sp, args) do {			\
	(toc)->serialize_private = sp;					\
	(toc)->serialize_args = args;					\
	(toc)->serialize = f;						\
	while ((toc)->serialize != NULL)				\
		__wt_yield();						\
} while (0)

/*
 * WT_TOC_GEN_IGNORE --
 *	A WT_TOC that's outside the library, or inside the library but known
 *	to be holding no resources.
 */
#define	WT_TOC_GEN_IGNORE	0

/*
 * WT_TOC_WAITER --
 *	The serialization reference for a WT_TOC waiting on another thread's
 *	request.
 */
#define	WT_TOC_WAITER		((void *)0x01)

/*
 * WT_TOC_SERIALIZE_WAIT --
 *	Wait on another thread's private serialization request.
 */
#define	WT_TOC_SERIALIZE_WAIT(toc, sp) do {				\
	while (*(sp) != 0 && *(sp) <= (toc)->api_gen) {			\
		(toc)->serialize = WT_TOC_WAITER;			\
		while (*(sp) != 0 && *(sp) <= (toc)->api_gen)		\
			__wt_yield();					\
		(toc)->serialize = NULL;				\
	}								\
} while (0)
/*
 * WT_TOC_SERIALIZE_VALUE --
 *	Wait on a memory location to go to zero.
 */
#define	WT_TOC_SERIALIZE_VALUE(toc, vp) do {				\
	WT_TOC_API_IGNORE(toc);						\
	while (*(vp) != 0) {						\
		(toc)->serialize = WT_TOC_WAITER;			\
		while (*(vp) != 0)					\
			__wt_yield();					\
		(toc)->serialize = NULL;				\
	}								\
	WT_TOC_API_RESET(toc);						\
} while (0)
