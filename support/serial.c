/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
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
 *	Configure a serialization request, and spin until it completes.
 */
int
__wt_toc_serialize_func(WT_TOC *toc, int (*func)(WT_TOC *), void *args)
{
	/*
	 * Threads serializing access to data using a function:
	 *	set a function/argument pair in the WT_TOC handle,
	 *	flush memory,
	 *	update the WT_TOC workq state,
	 *	flush memory, and
	 *	spins.
	 *
	 * The workQ thread notices the state change and calls the serialization
	 * function.  When the function returns, the workQ resets WT_TOC workq
	 * state, allowing the original thread to proceed.
	 *
	 * The first memory flush ensures all supporting information is written
	 * before the wq_state field (which makes the entry visible to the workQ
	 * thread).  No second memory flush is required, the wq_state field is
	 * declared volatile.
	 */
	toc->wq_args = args;
	toc->wq_func = func;
	WT_MEMORY_FLUSH;
	toc->wq_state = WT_WORKQ_FUNCTION;

	/* Spin on the WT_TOC->wq_state field. */
	while (toc->wq_state != WT_WORKQ_FUNCTION)
		__wt_yield();
	return (toc->wq_ret);
}

/*
 * __wt_toc_serialize_io --
 *	Configure an io request, and block until it completes.
 */
int
__wt_toc_serialize_io(WT_TOC *toc, u_int32_t addr, u_int32_t bytes)
{
	toc->wq_addr = addr;
	toc->wq_bytes = bytes;
	WT_MEMORY_FLUSH;
	toc->wq_state = WT_WORKQ_READ;

	/* Block until we're awakened. */
	__wt_lock(toc->env, toc->mtx);
	return (toc->wq_ret);
}
