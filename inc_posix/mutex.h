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
 * access to a piece of data, where the data (1) is accessed only by serialized
 * code, or where the data, when accessed by non-serialized code, can either
 * (2) be read atomically, or (3) it doesn't matter if it's read incorrectly.
 * In other words, the readers are key, and they are known to be indifferent
 * to the serialization code modifying the data.
 *
 * An example of #1 is updating the size of a database file.  The size is only
 * changed in serialized code, and never read by anything else.  An example of
 * #2 is updating a 32-bit value, because readers by definition get consistent
 * views of 32-bit memory locations.   An example of #3 is updating a 64-bit
 * value (such as the bytes allocated in the cache).  While there is a small
 * possibility a reader sees a corrupted value, the value is only used for
 * advisory actions, such as waking the cache thread to see if there's work to
 * do.
 *
 * Threads serializing access to a data set a function pointer/argument pair
 * in the WT_TOC handle, flush memory and spin; the workQ thread notes the set
 * and calls the function.  When the function returns, the workQ thread flushes
 * memory and clears the WT_TOC function pointer/argument pair, allowing the
 * original thread to proceed.  (The flush is not necessary: we cannot depend
 * on it, because serialization support doesn't lock other threads out of the
 * data, so there's no ordering constraint.  However, we flush so other threads
 * are as up-to-date as possible.)
 *
 * Private serialization:
 *
 * Additionally, serialization optionally supports data access where readers
 * are NOT indifferent to modifications of the data.   For example, a reader
 * might retrieve a page from the cache and be descheduled.   A writer of the
 * page must wait until the reader has discarded its reference to the page
 * before it modifies the page.
 *
 * This is supported by allowing a thread to request serialization, with the
 * additional requirement that serialization shouldn't occur until every thread
 * of control which might be in the process of reading the data has left the
 * library.
 *
 * Threads needing private serialization set the function pointer/argument as
 * described for serialization, and additionally set a reference to a structure
 * with two values: an API generation and a modification generation.
 *
 * The API generation is how we ensure potential readers have drained from the
 * library before the serialization function runs.  Threads with API generation
 * numbers equal-to-or-lower than the serialization point's continue through
 * the serialization point.  Threads with API generation numbers greater than
 * the point's value block.  When the workQ thread knows there are no lower API
 * generation numbers in the library than the set point's, the serialization
 * function is safe to run -- there can be no remaining readers of the data.
 *
 * This can stall.  For example, if two threads with the same API generation
 * request private serialization, they will wait forever, each waiting for the
 * other thread to leave the library.  To handle this, threads requesting
 * private serialization do not block other requests for private serialization.
 * This means no thread of control can access shared data, request private
 * serialization, and then re-access that same shared data, without some
 * higher-level lock in place to stop other threads from modifying the data,
 * or using some method to determine if the data has been modified while the
 * thread waited on its private serialization request.
 *
 * The modification generation value in the serialization point provides basic
 * support for detection data modification.  The modification generation is
 * incremented whenever a private serialization function runs.  Before running
 * a private serialization function, the workQ first checks the modification
 * generation.  If that value is unchanged since the private serialization
 * request was scheduled, the function runs.  If the value has changed, the
 * function isn't run and WT_RESTART is returned.
 */

/*
 * WT_TOC_SERIALIZE_REQUEST --
 *	Configure a serialization request.
 */
#define	WT_TOC_SERIALIZE_REQUEST(toc, f, sp, sa) do {			\
	(toc)->serial_private = sp;					\
	(toc)->serial_args = sa;					\
	(toc)->serial = f;						\
	while ((toc)->serial != NULL)					\
		__wt_yield();						\
} while (0)

/*
 * WT_TOC_SERIALIZE_WAIT --
 *	Cause a reader to wait on another thread's private serialization
 *	request, if the request's generation number is less than or equal
 *	to the reader's generation number.
 */
#define	WT_TOC_SERIALIZE_WAIT(toc, sp) do {				\
	while ((sp)->api_gen != 0 && (sp)->api_gen <= (toc)->api_gen) {	\
		F_SET((toc), WT_WAITING);				\
		while ((sp)->api_gen != 0 &&				\
		    (sp)->api_gen <= (toc)->api_gen)			\
			__wt_yield();					\
		F_CLR((toc), WT_WAITING);				\
	}								\
} while (0)

/*
 * WT_TOC_GEN_IGNORE --
 *	A WT_TOC that's outside the library, or inside the library but known
 *	to be holding no resources.
 */
#define	WT_TOC_GEN_IGNORE	0

/*
 * WT_TOC_SERIALIZE_VALUE --
 *	Cause a thread of control to wait on a memory location.   Here we
 *	don't care about API generation numbers, we only want to lock out
 *	threads for some reason.
 */
#define	WT_TOC_SERIALIZE_VALUE(toc, vp) do {				\
	WT_TOC_API_IGNORE(toc);						\
	while (*(vp) != 0) {						\
		F_SET((toc), WT_WAITING);				\
		while (*(vp) != 0)					\
			__wt_yield();					\
		F_CLR((toc), WT_WAITING);				\
	}								\
	WT_TOC_API_RESET(toc);						\
} while (0)
