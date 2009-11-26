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
__wt_env_open(ENV *env, const char *home, mode_t mode, u_int32_t flags)
{
	IENV *ienv;
	int ret;

	ienv = env->ienv;
	ret = 0;

	/* Create the cache. */
	WT_RET(__wt_cache_create(env));

	/* Turn on the methods that require open. */
	__wt_methods_env_open_transition(env);

	/* Start worker threads. */
	F_SET(ienv, WT_WORKQ_RUN | WT_SERVER_RUN);
	WT_MEMORY_FLUSH;

	WT_ERR(__wt_thread_create(&ienv->workq_tid, __wt_workq_srvr, env));
	WT_ERR(__wt_thread_create(&ienv->cache_tid, __wt_cache_srvr, env));

	return (0);

err:	(void)__wt_env_close(env, 0);
	return (ret);
}

/*
 * __wt_env_close --
 *	Close an Env handle.
 */
int
__wt_env_close(ENV *env, u_int32_t flags)
{
	IDB *idb;
	IENV *ienv;
	WT_FH *fh;
	int ret, secondary_err;

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

	/* Close down and wait for any server threads. */
	F_CLR(ienv, WT_SERVER_RUN);
	WT_MEMORY_FLUSH;

	__wt_unlock(&ienv->cache.mtx);
	__wt_thread_join(ienv->cache_tid);

	/*
	 * Close down and wait for the workQ thread; this only happens after
	 * all other server threads have exited, as they may be waiting on a
	 * request from the workQ.
	 */
	F_CLR(ienv, WT_WORKQ_RUN);
	WT_MEMORY_FLUSH;

	__wt_thread_join(ienv->workq_tid);

	/* Discard the cache. */
	WT_TRET(__wt_cache_destroy(env));

	/* Re-cycle the underlying ENV/IENV structures. */
	WT_TRET(__wt_ienv_destroy(env, 0));

	/* Free the Env structure. */
	memset(env, WT_OVERWRITE, sizeof(env));
	__wt_free(NULL, env);

	if (ret == 0)
		ret = secondary_err;

	return (ret == 0 ? secondary_err : ret);
}
