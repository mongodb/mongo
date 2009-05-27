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
 * wt_start --
 *	Start the engine.
 */
int
__wt_env_start(ENV *env, u_int32_t flags)
{
	WT_STOC *stoc;
	static int initial_tasks = 0;
	int ret;

	WT_ENV_FCHK(NULL, "wt_start", flags, WT_APIMASK_WT_START);

	/*
	 * No matter what we're doing, we end up here before we do any real
	 * work.   The first time, check the build itself and initialize the
	 * global structure.
	 */
	if (!initial_tasks) {
		if ((ret = __wt_build_verify()) != 0)
			return (ret);
		if ((ret = __wt_global_init()) != 0)
			return (ret);
		initial_tasks = 1;
	}

	WT_ENV_FCHK(NULL, "wt_start", flags, WT_APIMASK_WT_START);

	/* Create the primary thread-of-control structure. */
	stoc = WT_GLOBAL(sq) + WT_GLOBAL(sq_next);
	stoc->id = ++WT_GLOBAL(sq_next);

	__wt_stoc_init(env, stoc);

	/* If we're single-threaded, we're done. */
	if (LF_ISSET(WT_SINGLE_THREADED)) {
		WT_GLOBAL(single_threaded) = WT_GLOBAL(running) = 1;
		return (0);
	}

	/*
	 * The first thread is our house-keeping thread; spawn the engine,
	 * and wait until it's ready to proceed.
	 */
	if (pthread_create(&stoc->tid, NULL, __wt_workq, STOC_PRIME) != 0) {
		__wt_env_err(NULL, errno, "wt_start: house-keeping thread");
		return (WT_ERROR);
	}

	while (!WT_GLOBAL(running))
		__wt_sleep(0, 1000);

	return (0);
}

/*
 * wt_stop --
 *	Stop the engine.
 */
int
__wt_env_stop(ENV *env, u_int32_t flags)
{
	WT_STOC *stoc;
	u_int i;

	WT_ENV_FCHK(NULL, "wt_start", flags, WT_APIMASK_WT_STOP);

	/* If we're single-threaded, we're done. */
	if (WT_GLOBAL(single_threaded))
		return (0);

	/* Flag all threads to quit, and wait for them to exit. */
	WT_GLOBAL(running) = 0;
	WT_FLUSH_MEMORY;

	for (i = 0, stoc = WT_GLOBAL(sq); i < WT_GLOBAL(sq_next); ++i, ++stoc)
		(void)pthread_join(stoc->tid, NULL);

	return (0);
}

/*
 * __wt_stoc_init --
 *	Initialize the server thread-of-control structure.
 */
int
__wt_stoc_init(ENV *env, WT_STOC *stoc)
{
	u_int32_t i;
	int ret;

	/*
	 * Initialize the cache page queues.  Size for a cache filled with
	 * 16KB pages, and 8 pages per bucket (which works out to 8 buckets
	 * per MB).
	 */
	stoc->hashsize = __wt_prime(env->cachesize * 8);
	if ((ret = __wt_calloc(env,
	    stoc->hashsize, sizeof(stoc->hqh[0]), &stoc->hqh)) != 0)
		return (ret);
	for (i = 0; i < stoc->hashsize; ++i)
		TAILQ_INIT(&stoc->hqh[i]);
	TAILQ_INIT(&stoc->lqh);

	return (0);
}

/*
 * __wt_stoc_close --
 *	Clean out the server thread-of-control structure.
 */
int
__wt_stoc_close(ENV *env, WT_STOC *stoc)
{
	WT_PAGE *page;
	int ret, tret;

	ret = 0;

	/* Discard pages. */
	while ((page = TAILQ_FIRST(&stoc->lqh)) != NULL) {
		/* There shouldn't be any pinned pages. */
		WT_ASSERT(env, page->ref == 0);

		if ((tret =
		    __wt_cache_discard(env, stoc, page)) != 0 && ret == 0)
			ret = tret;
	}

	/* Discard buckets. */
	__wt_free(env, stoc->hqh);

	/* There shouldn't be any allocated bytes. */
	WT_ASSERT(env, stoc->cache_bytes == 0);

	return (ret);
}

/*
 * __wt_workq --
 *      Routine to process the work queue for a thread.
 */
void *
__wt_workq(void *arg)
{
	WT_STOC *stoc;
	WT_WORKQ *q, *eq;
	int not_found, tenths;

	stoc = arg;

	/* We're running. */
	WT_GLOBAL(running) = 1;
	WT_FLUSH_MEMORY;

	/* Walk the queue, executing work. */
	for (not_found = 1, tenths = 0,
	    q = WT_GLOBAL(workq), eq = q + WT_GLOBAL(workq_entries);;) {
		if (q->sid == stoc->id) {
			not_found = 0;

			F_SET(q->toc, WT_RUNNING);
			__wt_api_switch(q->toc);
			F_CLR(q->toc, WT_RUNNING);

			/*
			 * Ignore operations handed off to another server (the
			 * server ID will have changed).   Wake threads waiting
			 * on completed operations.
			 */
			if (q->sid == stoc->id) {
				q->sid = WT_PSTOC_NOT_SET;

				/*
				 * !!!
				 * No flush needed, we're unlocking a mutex.
				 */
				(void)__wt_unlock(q->toc->mtx);
			}
		}
		if (++q == eq) {
			/*
			 * If we didn't find work, yield the processor.  If
			 * we don't find work in 100 loops, start sleeping,
			 * from a 10th of a second to a second, by 10ths of
			 * a second.
			 */
			if (not_found == 0) {
				tenths = 0;
				not_found = 1;
			} else if (not_found >= 100) {
				if (tenths < 10)
					++tenths;
				__wt_sleep(0, tenths * 100000);
			} else {
				++not_found;
				__wt_yield();
			}
			q = WT_GLOBAL(workq);
		}
		if (WT_GLOBAL(running) == 0)
			break;
	}

	WT_FREE_AND_CLEAR(NULL, WT_GLOBAL(workq));

	return (NULL);
}
