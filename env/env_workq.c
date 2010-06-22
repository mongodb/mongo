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
	WT_FLIST *fp;
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
				read_priority =
				    F_ISSET(toc, WT_READ_PRIORITY) ? 1 : 0;
				break;
			}
		}

		/* Check if we can free some memory. */
		if ((fp = TAILQ_FIRST(&ienv->flistq)) != NULL &&
		    (low_gen == UINT32_MAX || low_gen > fp->gen))
			__wt_workq_flist(env);

		/* If a read is scheduled, check on the read server. */
		if (chk_read)
			__wt_workq_read_server(env, read_priority);

		/* Check on the cache drain server. */
		__wt_workq_drain_server(env);

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

	/* Free any remaining memory. */
	if ((fp = TAILQ_FIRST(&ienv->flistq)) != NULL)
		__wt_workq_flist(env);

	return (NULL);
}

/*
 * __wt_workq_repl --
 *	Swap in a new WT_PAGE replacement array.
 */
int
__wt_workq_repl(WT_TOC *toc, WT_REPL *orig, WT_REPL *new, WT_REPL **rp)
{
	WT_REPL *repl;

	repl = *rp;

	/*
	 * The WorkQ thread updates WT_REPL replacement structures, serializing
	 * changes to existing key/data items.   Callers have already allocated
	 * memory for the update and copied the previous structure's contents
	 * into that memory.
	 *
	 * If there's no existing replacement structure, ours is good, use it.
	 */
	if (repl != NULL) {
		/*
		 * If we don't reference the current replacement structure, it
		 * was replaced while we waited.   Our replacement is useless,
		 * discard it.
		 */
		if (orig != repl) {
			WT_FLIST_INSERT(
			    toc, new->data, new->repl_size * sizeof(WT_SDBT));
			WT_FLIST_INSERT(toc, new, sizeof(WT_REPL));

			/*
			 * Two threads might try to update the same item, and
			 * race while waiting for the WorkQ thread.  Even if
			 * we raced, if there's a free slot in the current
			 * WT_REPL structure (another thread updated the entry,
			 * and there's room for our caller's change), return
			 * success, our caller can use the empty slot.
			 */
			if (repl->data[repl->repl_next].data == NULL)
				return (0);

			return (WT_RESTART);
		}
	}

	if (repl != NULL) {
		WT_FLIST_INSERT(toc,
		    repl->data, repl->repl_size * sizeof(WT_SDBT));
		WT_FLIST_INSERT(toc, repl, sizeof(WT_REPL));
	}

	/*
	 * Update the WT_REPL structure and flush the change to the rest of the
	 * system.
	 */
	*rp = new;
	WT_MEMORY_FLUSH;

	return (0);
}
