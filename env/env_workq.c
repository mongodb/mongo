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
 * __wt_env_start --
 *	Start the engine.
 */
int
__wt_env_start(ENV *env, u_int32_t flags)
{
	IENV *ienv;
	WT_STOC *stoc;

	ienv = env->ienv;

	WT_ENV_FCHK(NULL, "Env.start", flags, WT_APIMASK_ENV_START);

	/*
	 * No matter what we're doing, we end up here before we do any real
	 * work.   Check the build itself.
	 */
	WT_RET((__wt_build_verify()));

	/* Create the primary thread-of-control structure. */
	stoc = ienv->sq  + ienv->sq_next;
	stoc->id = ++ienv->sq_next;
	stoc->running = 1;

	/* If we're single-threaded, we're done. */
	if (LF_ISSET(WT_SINGLE_THREADED)) {
		F_SET(ienv, WT_SINGLE_THREADED);
		return (0);
	}

	/* Spawn our primary thread. */
	if (pthread_create(&stoc->tid, NULL, __wt_workq, stoc) != 0) {
		__wt_env_err(env, errno, "Env.start: primary server thread");
		return (WT_ERROR);
	}

	/* We're running. */
	ienv->running = 1;
	WT_FLUSH_MEMORY;

	return (0);
}

/*
 * __wt_env_stop --
 *	Stop the engine.
 */
int
__wt_env_stop(ENV *env, u_int32_t flags)
{
	IENV *ienv;
	DB *db;
	WT_STOC *stoc;
	u_int i;

	ienv = env->ienv;

	WT_ENV_FCHK(NULL, "Env.stop", flags, WT_APIMASK_ENV_STOP);

	/*
	 * We don't close open databases -- we need a TOC to do that, and we
	 * don't have one.   Complain and kill any running threads.
	 */
	TAILQ_FOREACH(db, &env->dbqh, q)
		__wt_env_errx(env,
		    "Env.stop: database %s left open", db->idb->dbname);

	/* If we're single-threaded, we're done. */
	if (F_ISSET(ienv, WT_SINGLE_THREADED))
		return (0);

	/* Flag all running threads to quit, and wait for them to exit. */
	ienv->running = 0;
	WT_FLUSH_MEMORY;

	WT_STOC_FOREACH(ienv, stoc, i)
		if (stoc->running) {
			stoc->running = 0;
			WT_FLUSH_MEMORY;
			(void)pthread_join(stoc->tid, NULL);
		}

	return (0);
}

/*
 * __wt_workq --
 *      Routine to process the work queue for a thread.
 */
void *
__wt_workq(void *arg)
{
	IENV *ienv;
	WT_STOC *stoc;
	WT_TOC **q, **eq, *toc;
	int maxsleep, not_found;

	stoc = arg;
	ienv = stoc->idb->db->env->ienv;

	/* Walk the queue, executing work. */
	not_found = 1;
	q = stoc->ops;
	eq = q + sizeof(stoc->ops) / sizeof(stoc->ops[0]);
	do {
		if (*q != NULL) {			/* Operation. */
			WT_STAT_INCR(
			    stoc->stats, STOC_OPS, "server thread operations");

			toc = *q;
			not_found = 0;

			F_SET(toc, WT_RUNNING);
			__wt_api_switch(toc);
			F_CLR(toc, WT_RUNNING);

			/* Clear the TOC entry and wake the process. */
			*q = NULL;
			(void)__wt_unlock(toc->block);
		}

		if (++q == eq) {
			WT_STAT_INCR(stoc->stats,
			    STOC_ARRAY, "server thread array passes");
			/*
			 * If we didn't find work, yield the processor.
			 * If we don't find work in enough loops, sleep, where
			 * we gradually increase the sleep to 2 seconds.
			 */
			if (not_found++)
				if (not_found > 10) {
					if (not_found > 80)
						not_found = 40;
					WT_STAT_INCR(stoc->stats,
					    STOC_SLEEP, "server thread sleeps");
					__wt_sleep(0, not_found * 25000);
				} else {
					__wt_yield();
					WT_STAT_INCR(stoc->stats,
					    STOC_YIELD, "server thread yields");
				}
			q = stoc->ops;
		}
	} while (ienv->running == 1 && stoc->running == 1);

	return (NULL);
}
