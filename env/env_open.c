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
 * __wt_api_env_open --
 *	Open a Env handle.
 */
int
__wt_api_env_open(ENV *env, const char *home, mode_t mode, u_int32_t flags)
{
	IENV *ienv;

	ienv = env->ienv;

	WT_ENV_FCHK(env, "Env.open", flags, WT_APIMASK_ENV_OPEN);

	/* If we're not single-threaded, start the workQ thread. */
	if (!F_ISSET(ienv, WT_SINGLE_THREADED)) {
		F_SET(ienv, WT_RUNNING);
		WT_RET(__wt_thread_create(&ienv->primary_tid,__wt_workq, env));
	}

	/* Create the cache. */
	WT_RET(__wt_cache_create(env));

	/* Turn on the methods that require open. */
	__wt_methods_env_open_on(env);

	return (0);
}

/*
 * __wt_api_env_close --
 *	Close an Env handle.
 */
int
__wt_api_env_close(ENV *env, u_int32_t flags)
{
	IDB *idb;
	IENV *ienv;
	WT_FH *fh;
	int ret, secondary_err;

	ienv = env->ienv;
	ret = secondary_err = 0;

	WT_ENV_FCHK_NOTFATAL(
	    env, "ENV.close", flags, WT_APIMASK_ENV_CLOSE, ret);

	/* Close down the workQ server. */
	if (!F_ISSET(env, WT_SINGLE_THREADED)) {
		F_CLR(ienv, WT_RUNNING);
		WT_FLUSH_MEMORY;
		__wt_thread_join(ienv->primary_tid);
	}

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

	/* Discard the cache. */
	WT_TRET(__wt_cache_destroy(env));

	/* Re-cycle the underlying ENV/IENV structures. */
	WT_TRET(__wt_ienv_destroy(env, 0));

	/* Free the Env structure. */
	memset(env, OVERWRITE_BYTE, sizeof(env));
	__wt_free(NULL, env);

	if (ret == 0)
		ret = secondary_err;

	return (ret == 0 ? secondary_err : ret);
}
