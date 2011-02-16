/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

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
 * possibility a reader will see a corrupted value, the value is only used for
 * advisory actions, such as waking the cache thread to see if there's work to
 * do.
 */

/*
 * __wt_toc_serialize_func --
 *	Schedule a serialization request, and block or spin until it completes.
 */
int
__wt_toc_serialize_func(
    WT_TOC *toc, wq_state_t op, int spin, int (*func)(WT_TOC *), void *args)
{
	int done;

	/*
	 * Threads serializing access to data using a function:
	 *	set a function/argument pair in the WT_TOC handle,
	 *	flush memory,
	 *	update the WT_TOC workq state, and
	 *	spins or blocks.
	 *
	 * The workQ thread notices the state change and calls the serialization
	 * function.
	 *
	 * The first memory flush ensures all supporting information is written
	 * before the wq_state field (which makes the entry visible to the workQ
	 * thread).  No second memory flush is required, the wq_state field is
	 * declared volatile.
	 */
	toc->wq_args = args;
	toc->wq_func = func;
	toc->wq_sleeping = spin ? 0 : 1;
	WT_MEMORY_FLUSH;
	toc->wq_state = op;

	/*
	 * Callers can spin on the WT_TOC state (implying the call is quickly
	 * satisfied), or block until its mutex is unlocked by another thread
	 * when the operation has completed.
	 */
	if (spin) {
		/*
		 * !!!
		 * Don't do arithmetic comparisons (even equality) on enum's,
		 * it makes some compilers/lint tools angry.
		 */
		for (done = 0; !done;) {
			switch (toc->wq_state) {
			case WT_WORKQ_NONE:
				done = 1;
				break;
			case WT_WORKQ_FUNC:
			case WT_WORKQ_READ:
			case WT_WORKQ_READ_SCHED:
				__wt_yield();
				break;
			}
		}
	} else
		__wt_lock(toc->env, toc->mtx);

	return (toc->wq_ret);
}

/*
 * __wt_toc_serialize_wrapup --
 *	Server function cleanup.
 */
void
__wt_toc_serialize_wrapup(WT_TOC *toc, WT_PAGE *page, int ret)
{
	ENV *env;

	env = toc->env;

	/*
	 * If passed a page and the return value is good, we modified the page;
	 * no need for a memory flush, we'll use the one below.
	 */
	if (page != NULL && ret == 0)
		WT_PAGE_SET_MODIFIED(page);

	/*
	 * Set the return value and reset the state -- the workQ no longer needs
	 * to worry about us.
	 *
	 * The return value isn't volatile, so requires an explicit flush.
	 */
	toc->wq_ret = ret;
	toc->wq_state = WT_WORKQ_NONE;
	WT_MEMORY_FLUSH;

	/* If the calling thread is sleeping, wake it up. */
	if (toc->wq_sleeping)
		__wt_unlock(env, toc->mtx);
}
