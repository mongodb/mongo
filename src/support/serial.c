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
 * An example of #1 is updating the size of a file.  The size is only changed
 * in single-threaded code, and never read by anything else.  An example of #2
 * is updating a 32-bit value, because readers by definition get consistent
 * views of 32-bit memory locations.   An example of #3 is updating a 64-bit
 * value (such as the bytes allocated in the cache).  While there is a small
 * possibility a reader will see a corrupted value, the value is only used for
 * advisory actions, such as waking the cache thread to see if there's work to
 * do.
 */

/*
 * __wt_session_serialize_func --
 *	Schedule a serialization request, and block or spin until it completes.
 */
int
__wt_session_serialize_func(SESSION *session,
    wq_state_t op, int spin, int (*func)(SESSION *), void *args)
{
	int done;

	/*
	 * Threads serializing access to data using a function:
	 *	set a function/argument pair in the SESSION handle,
	 *	flush memory,
	 *	update the SESSION workq state, and
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
	session->wq_args = args;
	session->wq_func = func;
	session->wq_sleeping = spin ? 0 : 1;
	WT_MEMORY_FLUSH;
	session->wq_state = op;

	/*
	 * Callers can spin on the SESSION state (implying the call is quickly
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
			switch (session->wq_state) {
			case WT_WORKQ_NONE:
				done = 1;
				break;
			case WT_WORKQ_FUNC:
			case WT_WORKQ_EVICT:
			case WT_WORKQ_EVICT_SCHED:
			case WT_WORKQ_READ:
			case WT_WORKQ_READ_SCHED:
				__wt_yield();
				break;
			}
		}
	} else
		__wt_lock(session, session->mtx);

	return (session->wq_ret);
}

/*
 * __wt_session_serialize_wrapup --
 *	Server function cleanup.
 */
void
__wt_session_serialize_wrapup(SESSION *session, WT_PAGE *page, int ret)
{
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
	session->wq_ret = ret;
	session->wq_state = WT_WORKQ_NONE;
	WT_MEMORY_FLUSH;

	/* If the calling thread is sleeping, wake it up. */
	if (session->wq_sleeping)
		__wt_unlock(session, session->mtx);
}
