/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
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
	u_int32_t low_gen;
	int chk_read, nowork, read_priority;

	env = (ENV *)arg;
	ienv = env->ienv;
	nowork = 1;

	/* Walk the WT_TOC list and execute requests. */
	while (F_ISSET(ienv, WT_WORKQ_RUN)) {
		++ienv->api_gen;
		WT_STAT_INCR(ienv->stats, WORKQ_PASSES);

		low_gen = UINT32_MAX;
		chk_read = read_priority = 0;
		for (tp = ienv->toc; (toc = *tp) != NULL; ++tp) {
			if (toc->gen < low_gen)
				low_gen = toc->gen;
			switch (toc->wq_state) {
			case WT_WORKQ_NONE:
				continue;
			case WT_WORKQ_SPIN:
				/*
				 * Call the function, flush out the results,
				 * then let the thread proceed.
				 */
				toc->wq_ret = toc->wq_func(toc);
				WT_MEMORY_FLUSH;
				toc->wq_state = WT_WORKQ_NONE;

				nowork = 0;
				break;
			case WT_WORKQ_READ:
				/*
				 * Call the function, flush out the results.
				 * If the call failed, wake the waiting thread,
				 * otherwise update the state so we'll check
				 * the I/O thread as necessary.
				 */
				toc->wq_ret = toc->wq_func(toc);
				WT_MEMORY_FLUSH;
				if (toc->wq_ret != 0) {
					toc->wq_state = WT_WORKQ_NONE;
					__wt_unlock(env, toc->mtx);
					break;
				}
				toc->wq_state = WT_WORKQ_READ_SCHED;
				nowork = 0;
				/* FALLTHROUGH */
			case WT_WORKQ_READ_SCHED:
				chk_read = 1;
				if (F_ISSET(toc, WT_READ_PRIORITY))
					read_priority = 1;
				break;
			}
		}

		/* If a read is scheduled, check on the read server. */
		if (chk_read)
			__wt_workq_read_server(env, read_priority ? 1 : 0);

		/* Check on the cache drain server. */
		__wt_workq_drain_server(env, 0);

		/*
		 * If we didn't find work, yield the processor.  If we
		 * don't find work for awhile, sleep.
		 */
		if (nowork++) {
			if (nowork >= 100000) {
				WT_STAT_INCR(ienv->stats, WORKQ_SLEEP);
				__wt_sleep(0, nowork);
				nowork = 100000;
			} else {
				WT_STAT_INCR(ienv->stats, WORKQ_YIELD);
				__wt_yield();
			}
		}
	}

	return (NULL);
}
