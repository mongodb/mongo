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
 *
 * Threads serializing access to data set a function pointer/argument pair in
 * the WT_TOC handle, flush memory and spin; the workQ thread notes the set
 * and calls the function.  When the function returns, the workQ thread clears
 * the WT_TOC function pointer pair and flushes memory, allowing the original
 * thread to proceed.
 *
 * Private serialization:
 *
 * Additionally, serialization optionally supports data access where readers
 * are NOT indifferent to modifications of the data.   For example, a reader
 * might retrieve a page from the cache and be descheduled.   A writer of the
 * page must wait until the reader has discarded its reference to the page
 * before changing the page.
 *
 * This is supported by allowing a thread to request "private serialization",
 * which is serialization with the additional requirement that the function
 * should not be called until every thread of control which might be in the
 * process of reading the data has left the library.
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
 * support for detecting data modification.  The modification generation is
 * incremented whenever a private serialization function runs.  Before running
 * a private serialization function, the workQ first checks the modification
 * generation.  If that value is unchanged since the private serialization
 * request was scheduled, the function runs.  If the value has changed, the
 * function isn't run and WT_RESTART is returned.
 */

 /*
  * __wt_toc_serialize_request --
  *	Configure a serialization request, and wait for it.
  */
void
__wt_toc_serialize_request(
    WT_TOC *toc, int (*serial)(WT_TOC *), void *args, WT_SERIAL *serial_private)
{
	toc->serial_args = args;
	toc->serial_private = serial_private;

	/*
	 * The assignment to WT_TOC->serial makes the request visible to the
	 * workQ.
	 */
	toc->serial = serial;
	WT_MEMORY_FLUSH;

	/* We're spinning on the WT_TOC->serial field, it's marked volatile. */
	while (toc->serial != NULL)
		__wt_yield();
}

/*
 * __wt_toc_serialize_wait --
 *	Wait on another thread's private serialization request.
 */
void
__wt_toc_serialize_wait(WT_TOC *toc, WT_SERIAL *serial_private)
{
	if (serial_private == NULL) {
		/*
		 * Whatever operation decided we should block set the toc's
		 * serialize_private reference to point to the memory location
		 * blocking us.   If they didn't set that reference, then the
		 * memory location is going away, just retry the operation.
		 */
		if ((serial_private = toc->serial_private) == NULL)
			return;
	} else
		toc->serial_private = serial_private;

	/*
	 * Wait on another thread's private serialization request, if the
	 * if the request's generation number is less than or equal to the
	 * our generation number.
	 *
	 * The test is simplified: if the serialization point has an api_gen
	 * of 0, we'll go right through; if the serialization point has an
	 * api_gen of 1, we'll block until the value is cleared, because a
	 * WT_TOC never has an api_gen that low.
	 *
	 * The WT_TOC->flags field is not volatile, the WT_WAITING flag may
	 * or may not be set, and is only used for debugging.
	 *
	 * We're spinning on the WT_SERIAL->api_gen field, it's marked volatile.
	 */
	if (serial_private->api_gen > 0 &&
	    serial_private->api_gen <= toc->api_gen) {
		F_SET(toc, WT_WAITING);
		while (serial_private->api_gen > 0 &&
		    serial_private->api_gen <= toc->api_gen)
			__wt_yield();
		F_CLR(toc, WT_WAITING);
	}
}
