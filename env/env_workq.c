/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static void __wt_queue_cache_check(ENV *);
static void __wt_queue_op_check(ENV *);

/*
 * __wt_workq_srvr --
 *      Routine to process the WT_TOC work queue.
 */
void *
__wt_workq_srvr(void *arg)
{
	ENV *env;
	IENV *ienv;
	WT_TOC *toc;
	u_int32_t low_api_gen;
	int flush, notfound;

	env = (ENV *)arg;
	ienv = env->ienv;

	flush = 0;
	notfound = 1;
	while (F_ISSET(ienv, WT_WORKQ_RUN)) {
		/*
		 * Walk the WT_TOC queue and: execute non-private serialization
		 * requests, see if any new private seialization requests have
		 * been scheduled, find out the lowest API generation number of
		 * any thread in the system that's running unblocked.
		 */
		low_api_gen = 0;
		TAILQ_FOREACH(toc, &ienv->tocqh, q) {
			/*
			 * Only review API generation numbers for entries that
			 * are running in the library.
			 */
			if (toc->api_gen != WT_TOC_GEN_IGNORE &&
			    toc->serial_private == NULL &&
			    (low_api_gen == 0 || low_api_gen > toc->api_gen)) {
				low_api_gen = toc->api_gen;
			}

			/* We're done with threads not waiting for anything. */
			if (toc->serial == NULL)
				continue;

			/*
			 * See if there's a new private serialization request.
			 * That's a WT_TOC with a serialization point set, but
			 * no API generation number to go with it.
			 *
			 * Flush the current API generation number plus 1 into
			 * the serialization point.  Then, increment the API
			 * generation number, and flush that operation.  This
			 * dance ensures that any thread with an API generation
			 * number larger than the serialization point's value
			 * is NOT a problem, because the thread will be blocked
			 * before it can access the data structure.
			 *
			 * There are no ordering issues for the modification
			 * generation value.   It's checked (and updated) when
			 * the private serialization function runs, and there's
			 * always a flush between the update and the check.
			 *
			 * It's OK if multiple threads request the same field.
			 * The first one we find in the list sets the api_gen
			 * field, and the first one we find in the list will
			 * run when the field becomes available, and then the
			 * process will repeat.  Starvation is theoretically
			 * possible, but I'm not going to worry about it until
			 * I see it.
			 */
			if (toc->serial_private != NULL) {
				if (toc->serial_private->api_gen == 0) {
					toc->mod_gen =
					    toc->serial_private->mod_gen;
					toc->serial_private->api_gen =
					    ienv->api_gen + 1;
					WT_MEMORY_FLUSH;
					++ienv->api_gen;
					WT_MEMORY_FLUSH;
				}
				continue;
			}

			/*
			 * In the first pass, we only run serializations that
			 * aren't private requests.  This is because we have
			 * to figure out the lowest-API-generation value of
			 * any thread running free before granting private
			 * serialization requests.  Satisfy simple requests.
			 */
			toc->serial_ret = toc->serial(toc);
			toc->serial = NULL;
			flush = 1;
			notfound = 0;
		}

		/* Flush out changes so other threads can proceed. */
		if (flush) {
			flush = 0;
			WT_MEMORY_FLUSH;
		}

		/*
		 * Walk the WT_TOC queue and execute private serialization
		 * requests for which there are no free-running threads left
		 * that could be a problem.   We also execute non-private
		 * serialization requests if we find them, there's no reason
		 * to waste the queue walk.
		 */
		TAILQ_FOREACH(toc, &ienv->tocqh, q) {
			/* Skip threads not waiting for anything. */
			if (toc->serial == NULL)
				continue;

			/* Run serialization requests */
			if (toc->serial_private == NULL)
				toc->serial_ret = toc->serial(toc);
			else {
				/*
				 * Skip private requests appearing in the queue
				 * after the previous loop (we haven't yet set
				 * the serialization point's api_gen value).
				 */
				if (toc->serial_private->api_gen == 0)
					continue;

				/*
				 * Skip threads that can't yet run because there
				 * remain threads in the system with lower API
				 * generation numbers.
				 */
				if (low_api_gen != 0 &&
				    toc->serial_private->api_gen > low_api_gen)
					continue;

				/*
				 * Reject private requests where data has been
				 * modified since the request was scheduled.
				 */
				if (toc->mod_gen ==
				    toc->serial_private->mod_gen) {
					toc->serial_ret = toc->serial(toc);
					toc->serial_private->api_gen = 0;
					toc->serial_private->mod_gen++;
				} else {
					toc->serial_ret = WT_RESTART;
					WT_STAT_INCR(
					    ienv->stats, WORKQ_RESTARTS,
					    "workQ modify restarted operation");
				}
				toc->serial_private = NULL;
			}
			toc->serial = NULL;
			flush = 1;
			notfound = 0;
		}

		/* Flush out changes so other threads can proceed. */
		if (flush) {
			flush = 0;
			WT_MEMORY_FLUSH;
		}

		__wt_queue_op_check(env);
		__wt_queue_cache_check(env);

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

	/*
	 * One final check, in case we race with the thread telling us to
	 * stop.
	 */
	__wt_queue_op_check(env);

	return (NULL);
}

/*
 * __wt_queue_op_check --
 *	Check for ENV queue operations.
 */
static void
__wt_queue_op_check(ENV *env)
{
	IENV *ienv;

	ienv = env->ienv;

	/* Check to see if there's a WT_TOC queue operation waiting. */
	if (ienv->toc_add != NULL) {
		TAILQ_INSERT_TAIL(&ienv->tocqh, ienv->toc_add, q);
		ienv->toc_add = NULL;
		__wt_unlock(&ienv->mtx);
	}
	if (ienv->toc_del != NULL) {
		TAILQ_REMOVE(&ienv->tocqh, ienv->toc_del, q);
		__wt_free(env, ienv->toc_del);
		ienv->toc_del = NULL;
		__wt_unlock(&ienv->mtx);
	}
}

/*
 * __wt_queue_cache_check --
 *	Check for the cache filling up.
 */
static void
__wt_queue_cache_check(ENV *env)
{
	IENV *ienv;
	WT_CACHE *cache;

	ienv = env->ienv;
	cache = &ienv->cache;

	/* Wake the server if it's sleeping and we need it to run. */
	if (F_ISSET(cache, WT_SERVER_SLEEPING) &&
	    cache->bytes_alloc > cache->bytes_max) {
		F_CLR(cache, WT_SERVER_SLEEPING);
		WT_MEMORY_FLUSH;
		__wt_unlock(&cache->mtx);
	}

	/*
	 * If we're 10% over the maximum cache, lock down the API until we
	 * drain to at least 5% under the maximum cache.
	 */
	if (cache->bytes_alloc > cache->bytes_max + (cache->bytes_max / 10)) {
		if (ienv->cache_lockout.api_gen != 0) {
			WT_STAT_INCR(
			    ienv->stats, CACHE_LOCKOUT, "API cache lockout");
			ienv->cache_lockout.api_gen = WT_TOC_GEN_BLOCK;
			WT_MEMORY_FLUSH;
		}
	} else if (ienv->cache_lockout.api_gen != 0 &&
	    cache->bytes_alloc <= cache->bytes_max - (cache->bytes_max / 20)) {
		ienv->cache_lockout.api_gen = 0;
		WT_MEMORY_FLUSH;
	}
}
