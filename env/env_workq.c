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
		WT_STAT_INCR(ienv->stats, WORKQ_PASSES, "workQ queue passes");
		if (notfound++)
			if (notfound >= 100000) {
				WT_STAT_INCR(ienv->stats,
				    WORKQ_SLEEP, "workQ sleeps");
				__wt_sleep(0, notfound);
				notfound = 100000;
			} else {
				WT_STAT_INCR(ienv->stats,
				    WORKQ_YIELD, "workQ yields");
				__wt_yield();
			}
	}

	return (NULL);
}
