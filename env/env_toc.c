/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_env_toc_destroy(WT_TOC *, u_int32_t);

/*
 * wt_toc_create --
 *	WT_TOC constructor.
 */
int
__wt_env_toc_create(ENV *env, u_int32_t flags, WT_TOC **tocp)
{
	IENV *ienv;
	WT_TOC *toc;
	int ret;

	ienv = env->ienv;

	WT_ENV_FCHK(env, "wt_toc_create", flags, WT_APIMASK_ENV_TOC_CREATE);

	WT_RET(__wt_calloc(env, 1, sizeof(WT_TOC), &toc));
	WT_ERR(__wt_calloc(env, 1, sizeof(WT_MTX), &toc->block));

	/* The mutex is self-blocking, so it's normal state is locked. */
	WT_ERR(__wt_mtx_init(toc->block));
	WT_ERR(__wt_lock(toc->block));

	/* Get a server slot ID. */
	WT_ERR(__wt_lock(&ienv->mtx));
	toc->slot = ienv->toc_slot++;
	WT_ERR(__wt_unlock(&ienv->mtx));
	if (toc->slot >= WT_SERVER_QSIZE) {
		__wt_env_errx(env, "wt_env_toc_create: too many threads");
		ret = WT_ERROR;
		goto err;
	}

	if (F_ISSET(ienv, WT_SINGLE_THREADED))
		F_SET(toc, WT_SINGLE_THREADED);
	toc->destroy = __wt_env_toc_destroy;

	*tocp = toc;
	return (0);

err:	(void)__wt_env_toc_destroy(toc, 0);
	return (ret);
}

/*
 * __wt_toc_destroy --
 *	toc.destroy method (WT_TOC destructor).
 */
static int
__wt_env_toc_destroy(WT_TOC *toc, u_int32_t flags)
{
	ENV *env;
	int ret;

	env = toc->env;
	ret = 0;

	WT_ENV_FCHK_NOTFATAL(
	    env, "WtToc.destroy", flags, WT_APIMASK_TOC_DESTROY, ret);

	WT_TRET(__wt_mtx_destroy(toc->block));

	WT_FREE_AND_CLEAR(env, toc->block);

	__wt_free(env, toc);

	return (ret);
}

/*
 * __wt_env_toc_sched --
 *	Schedule an operation.
 */
int
__wt_env_toc_sched(WT_TOC *toc)
{
	WT_STOC *stoc;

	/*
	 * The engine may be single-threaded or threads of control may re-enter
	 * the API.  Instead of coding internal versions of the routines, allow
	 * calls through the standard API layer.
	 */
	if (F_ISSET(toc, WT_RUNNING | WT_SINGLE_THREADED)) {
		__wt_api_switch(toc);
		return (toc->ret);
	}

	/*
	 * Otherwise, schedule the call and go to sleep until it completes.
	 *
	 * Decide what server to use: use the specified server if doing a DB
	 * handle operation, all other calls are handled by the primary server.
	 */
	if (toc->db == NULL || (stoc = toc->db->idb->stoc) == NULL)
		stoc = toc->env->ienv->sq;

	stoc->ops[toc->slot] = toc;

	(void)__wt_lock(toc->block);

	/* Return the operation's code. */
	return (toc->ret);
}
