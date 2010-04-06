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
 * __wt_env_open --
 *	Open a Env handle.
 */
int
__wt_env_open(ENV *env, const char *home, mode_t mode)
{
	IENV *ienv;
	int ret;

	WT_CC_QUIET(home, NULL);
	WT_CC_QUIET(mode, 0);

	ienv = env->ienv;
	ret = 0;

	/* Create the cache. */
	WT_RET(__wt_cache_create(env));

	/* Transition to the open state. */
	__wt_methods_env_open_transition(env);

	/* Start worker threads. */
	F_SET(ienv, WT_WORKQ_RUN | WT_SERVER_RUN);
	WT_MEMORY_FLUSH;

	WT_ERR(
	    __wt_thread_create(&ienv->cache_drain_tid, __wt_cache_drain, env));
	WT_ERR(__wt_thread_create(&ienv->cache_io_tid, __wt_cache_io, env));
	WT_ERR(__wt_thread_create(&ienv->workq_tid, __wt_workq_srvr, env));

	return (0);

err:	(void)__wt_env_close(env);
	return (ret);
}

/*
 * __wt_env_close --
 *	Close an Env handle.
 */
int
__wt_env_close(ENV *env)
{
	IDB *idb;
	IENV *ienv;
	WT_FH *fh;
	int ret, secondary_err;

#ifdef HAVE_DIAGNOSTIC
	if (F_ISSET(env, ~WT_APIMASK_ENV))
		(void)__wt_api_args(env, "Env.close");
#endif

	ienv = env->ienv;
	ret = secondary_err = 0;

	/* Complain if DB handles weren't closed. */
	if (TAILQ_FIRST(&ienv->dbqh) != NULL) {
		TAILQ_FOREACH(idb, &ienv->dbqh, q) {
			__wt_api_env_errx(env,
			    "Env handle has open Db handles: %s",
			    idb->dbname);
			WT_TRET(idb->db->close(idb->db, 0));
		}
		secondary_err = WT_ERROR;
	}

	/* Complain if files weren't closed. */
	if (TAILQ_FIRST(&ienv->fhqh) != NULL) {
		TAILQ_FOREACH(fh, &ienv->fhqh, q) {
			__wt_api_env_errx(env,
			    "Env handle has open file handles: %s",
			    fh->name);
			WT_TRET(__wt_close(env, fh));
		}
		secondary_err = WT_ERROR;
	}

	/* Close down and wait for server threads. */
	F_CLR(ienv, WT_SERVER_RUN);
	WT_MEMORY_FLUSH;
	__wt_unlock(ienv->cache->mtx_drain);
	__wt_thread_join(ienv->cache_drain_tid);
	__wt_unlock(ienv->cache->mtx_io);
	__wt_thread_join(ienv->cache_io_tid);

	/*
	 * Close down and wait for the workQ thread; this only happens after
	 * all other server threads have exited, as they may be waiting on a
	 * request from the workQ, or vice-versa.
	 */
	F_CLR(ienv, WT_WORKQ_RUN);
	WT_MEMORY_FLUSH;
	__wt_thread_join(ienv->workq_tid);

	/* Discard the cache. */
	WT_TRET(__wt_cache_destroy(env));

	/* Re-cycle the underlying ENV/IENV structures. */
	WT_TRET(__wt_ienv_destroy(env, 0));

	/* Free the Env structure. */
	__wt_free(NULL, env, sizeof(ENV));

	if (ret == 0)
		ret = secondary_err;

	return (ret == 0 ? secondary_err : ret);
}
