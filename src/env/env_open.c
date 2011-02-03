/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
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

	/* WT_TOC and hazard arrays. */
	WT_RET(__wt_calloc(env, env->toc_size, sizeof(WT_TOC *), &ienv->toc));
	WT_RET(
	    __wt_calloc(env, env->toc_size, sizeof(WT_TOC), &ienv->toc_array));
	WT_RET(__wt_calloc(env,
	   env->toc_size * env->hazard_size, sizeof(WT_PAGE *), &ienv->hazard));

	/* Create the cache. */
	WT_RET(__wt_cache_create(env));

	/* Transition to the open state. */
	__wt_methods_env_open_transition(env);

	/* Start worker threads. */
	F_SET(ienv, WT_WORKQ_RUN | WT_SERVER_RUN);
	WT_MEMORY_FLUSH;

	WT_ERR(__wt_thread_create(
	    &ienv->cache_evict_tid, __wt_cache_evict_server, env));
	WT_ERR(__wt_thread_create(
	    &ienv->cache_read_tid, __wt_cache_read_server, env));
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

	WT_ENV_FCHK_RET(env, "Env.close", env->flags, WT_APIMASK_ENV, ret);

	ienv = env->ienv;
	ret = secondary_err = 0;

	/* Complain if DB handles weren't closed. */
	while ((idb = TAILQ_FIRST(&ienv->dbqh)) != NULL) {
		__wt_api_env_errx(env,
		    "Env handle has open Db handles: %s", idb->name);
		WT_TRET(idb->db->close(idb->db, 0));
		secondary_err = WT_ERROR;
	}

	/* Complain if files weren't closed. */
	while ((fh = TAILQ_FIRST(&ienv->fhqh)) != NULL) {
		__wt_api_env_errx(env,
		    "Env handle has open file handles: %s", fh->name);
		WT_TRET(__wt_close(env, fh));
		secondary_err = WT_ERROR;
	}

	/* Shut down the server threads. */
	F_CLR(ienv, WT_SERVER_RUN);
	WT_MEMORY_FLUSH;

	/*
	 * Force the cache server threads to run and wait for them to exit.
	 * Wait for the cache eviction server first, it potentially schedules
	 * work for the read thread.
	 */
	__wt_workq_evict_server(env, 1);
	__wt_thread_join(ienv->cache_evict_tid);
	__wt_workq_read_server(env, 1);
	__wt_thread_join(ienv->cache_read_tid);

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
	WT_TRET(__wt_ienv_destroy(env));

	/* Free the Env structure. */
	__wt_free(NULL, env, sizeof(ENV));

	if (ret == 0)
		ret = secondary_err;

	return (ret == 0 ? secondary_err : ret);
}
