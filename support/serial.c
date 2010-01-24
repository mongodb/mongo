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
 */
 /*
  * __wt_toc_serialize_request --
  *	Configure a serialization request, and wait for it.
  */
void
__wt_toc_serialize_request(WT_TOC *toc, int (*serial)(WT_TOC *), void *args)
{
	/*
	 * The assignment to WT_TOC->serial makes the request visible to the
	 * workQ.  If we're not passed a serial function, it just means the
	 * caller has made other arrangements with the workQ, and the function
	 * will never be called.  Fill in a function that will die horribly
	 * if it's ever called.
	 */
	toc->serial_args = args;
	toc->serial = serial == NULL ? (int (*)(WT_TOC *))__wt_abort : serial;
	WT_MEMORY_FLUSH;

	/* We're spinning on the WT_TOC->serial field, it's marked volatile. */
	while (toc->serial != NULL)
		__wt_yield();
}
