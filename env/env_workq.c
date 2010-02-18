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
 * __wt_workq_srvr --
 *      Routine to process the WT_TOC work queue.
 */
void *
__wt_workq_srvr(void *arg)
{
	ENV *env;
	IENV *ienv;
	WT_TOC **tp, *toc;
	int notfound;

	env = (ENV *)arg;
	ienv = env->ienv;

	notfound = 1;
	while (F_ISSET(ienv, WT_WORKQ_RUN)) {
		/* Walk the WT_TOC list and execute serialization requests. */
		for (tp = ienv->toc; (toc = *tp) != NULL; ++tp) {
			if (toc->serial == NULL)
				continue;

			/* The thread may be waiting on the cache to drain. */
			if (F_ISSET(toc, WT_CACHE_DRAIN_WAIT)) {
				if (F_ISSET(ienv, WT_CACHE_LOCKOUT))
					continue;
				toc->serial_ret = 0;
			} else
				toc->serial_ret = toc->serial(toc);

			toc->serial = NULL;
			WT_MEMORY_FLUSH;
			notfound = 0;
		}

		__wt_cache_size_check(env);

		/*
		 * If we didn't find work, yield the processor.  If we
		 * don't find work for awhile, sleep.
		 */
		WT_STAT_INCR(ienv->stats, WORKQ_PASSES);
		if (notfound++) {
			if (notfound >= 100000) {
				WT_STAT_INCR(ienv->stats, WORKQ_SLEEP);
				__wt_sleep(0, notfound);
				notfound = 100000;
			} else {
				WT_STAT_INCR(ienv->stats, WORKQ_YIELD);
				__wt_yield();
			}
		}
	}

	return (NULL);
}

/*
 * __wt_workq_repl --
 *	Swap in a new WT_PAGE replacement array.
 */
int
__wt_workq_repl(ENV *env, WT_REPL **orig, WT_REPL *new)
{
	WT_REPL *repl;
	int ret;

	repl = *orig;
	ret = 0;

	/*
	 * The WorkQ thread updates the WT_REPL replacement structure so writes
	 * to an existing key/data item are serialized.   Callers have already
	 * allocated memory for the update and copied the previous replacement
	 * structure's contents into that memory.
	 *
	 * It's easy the first time -- just fill stuff in.
	 */
	if (repl == NULL) {
		*orig = new;
		return (0);
	}

	/*
	 * Two threads might try to update the same item, and race while waiting
	 * for the WorkQ thread.   Before updating the replacement structure, we
	 * confirm the current WT_REPL reference is the same as when we decided
	 * to do the update.
	 *
	 * Even if we raced, if there's an empty slot in the current WT_REPL
	 * structure (another thread did an update, and there's room for our
	 * caller's change), we return success, our caller can use that empty
	 * slot, there's no need to upgrade the WT_REPL structure again.
	 */
	if (repl->data[repl->repl_next].data == NULL)
		goto err;
	if (new->next != repl) {
		ret = WT_RESTART;

err:		__wt_free(env, new->data, 0);
		__wt_free(env, new, sizeof(WT_REPL));
		return (ret);
	}


	/*
	 * Update the WT_REPL structure and flush the change to the rest of the
	 * system.
	 */
	*orig = new;
	WT_MEMORY_FLUSH;

	return (0);
}
