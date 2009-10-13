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
	extern u_int __wt_cthread_count;
	WT_SRVR *srvr;
	IENV *ienv;

	ienv = env->ienv;

	WT_ENV_FCHK(NULL, "Env.start", flags, WT_APIMASK_ENV_START);

	if (LF_ISSET(WT_SINGLE_THREADED))
		F_SET(ienv, WT_SINGLE_THREADED);

	/*
	 * No matter what we're doing, we end up here before we do any real
	 * work.   Check the build itself, and do some global stuff.
	 */
	WT_RET(__wt_build_verify());

	/* Initialize the primary thread-of-control structure. */
	srvr = &ienv->psrvr;
	srvr->id = WT_SRVR_PRIMARY;
	srvr->env = env;
	WT_RET(__wt_calloc(env,
	    __wt_cthread_count, sizeof(WT_TOC_CACHELINE), &srvr->ops));
	WT_RET(__wt_stat_alloc_srvr_stats(env, &srvr->stats));
	if (!F_ISSET(ienv, WT_SINGLE_THREADED))
		WT_RET(__wt_thread_create(&srvr->tid, __wt_workq, srvr));

	return (0);
}

/*
 * __wt_env_stop --
 *	Stop the engine.
 */
int
__wt_env_stop(ENV *env, u_int32_t flags)
{
	IDB *idb;
	IENV *ienv;
	WT_SRVR *srvr;
	WT_TOC *toc;
	int ret;

	ienv = env->ienv;
	ret = 0;

	WT_ENV_FCHK(NULL, "Env.stop", flags, WT_APIMASK_ENV_STOP);

	/*
	 * Close any open databases -- we need a WT_TOC to call into the
	 * DB handle functions, create one as necessary.
	 */
	toc = NULL;
	TAILQ_FOREACH(idb, &ienv->dbqh, q) {
		if (toc == NULL)
			WT_RET(env->toc_create(env, 0, &toc));
		WT_TRET(idb->db->close(idb->db, toc, 0));
	}

	if (toc != NULL)
		WT_TRET(toc->destroy(toc, 0));

	/* Close down the primary server. */
	srvr = &ienv->psrvr;
	srvr->running = 0;
	WT_FLUSH_MEMORY;

	__wt_free(env, srvr->ops);
	__wt_free(env, srvr->stats);

	if (!F_ISSET(ienv, WT_SINGLE_THREADED))
		__wt_thread_join(srvr->tid);

	return (ret);
}

/*
 * __wt_workq --
 *      Routine to process the work queue for a thread.
 */
void *
__wt_workq(void *arg)
{
	extern u_int __wt_cthread_count;
	ENV *env;
	WT_SRVR *srvr;
	WT_TOC_CACHELINE *q, *eq;
	WT_TOC *toc;
	int notfound;

	srvr = arg;
	env = srvr->env;
	srvr->running = 1;

	if (FLD_ISSET(env->verbose, WT_VERB_SERVERS))
		__wt_env_errx(env, "server %d starting", srvr->id);

	/* Walk the queue, executing work. */
	notfound = 1;
	q = srvr->ops;
	eq = q + __wt_cthread_count;
	do {
		if (q->toc != NULL) {			/* Operation. */
			WT_STAT_INCR(
			    srvr->stats, SRVR_OPS, "server thread operations");

			notfound = 0;

			/*
			 * Save and clear the WT_TOC. (Clear it now, as soon as
			 * we wake the thread that scheduled this job, it can
			 * schedule a new job, and we need to get ahead of that
			 * memory write.)
			 */
			toc = q->toc;

			/* The operation uses this server's cache; set it. */
			WT_TOC_SET_CACHE(toc, srvr);

			/* Perform the operation. */
			F_SET(toc, WT_RUNNING);
			__wt_api_switch(toc);
			F_CLR(toc, WT_RUNNING);

			/* Unblock the client thread. */
			q->toc = NULL;
			WT_FLUSH_MEMORY;
		}

		if (++q == eq) {
			WT_STAT_INCR(srvr->stats,
			    SRVR_ARRAY, "server thread array passes");
			/*
			 * If we didn't find work, yield the processor.  If we
			 * don't find work for awhile, sleep.
			 */
			if (notfound++)
				if (notfound >= 100000) {
					WT_STAT_INCR(srvr->stats,
					    SRVR_SLEEP, "server thread sleeps");
					__wt_sleep(0, notfound);
					notfound = 100000;
				} else {
					WT_STAT_INCR(srvr->stats,
					    SRVR_YIELD, "server thread yields");
					__wt_yield();
				}
			q = srvr->ops;
		}
	} while (srvr->running == 1);

	if (FLD_ISSET(env->verbose, WT_VERB_SERVERS))
		__wt_env_errx(env, "server %d exiting", srvr->id);

	return (NULL);
}
