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
	WT_TOC *toc;
	int ret;

	WT_ENV_FCHK(env, "wt_toc_create", flags, WT_APIMASK_WT_TOC_CREATE);

	if ((ret = __wt_calloc(env, 1, sizeof(WT_TOC), &toc)) != 0)
		return (ret);
	if ((ret = __wt_calloc(env, 1, sizeof(WT_MTX), &toc->block)) != 0)
		goto err;

	/* The mutex is self-blocking, so it's normal state is locked. */
	if ((ret = __wt_mtx_init(toc->block)) != 0)
		goto err;
	if ((ret = __wt_lock(toc->block)) != 0)
		goto err;

	/* Get a server slot ID. */
	if ((ret = __wt_lock(&WT_GLOBAL(mtx))) != 0)
		goto err;
	toc->slot = WT_GLOBAL(toc_slot)++;
	if (toc->slot >= WT_SERVER_QSIZE) {
		__wt_db_errx(NULL, "wt_env_toc_create: too many threads");
		ret = WT_ERROR;
		goto err;
	}
	if ((ret = __wt_unlock(&WT_GLOBAL(mtx))) != 0)
		goto err;

	if (WT_GLOBAL(single_threaded))
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
	int ret, tret;

	env = toc->env;
	ret = 0;

	WT_ENV_FCHK_NOTFATAL(
	    env, "WtToc.destroy", flags, WT_APIMASK_WT_TOC_DESTROY, ret);

	if ((tret = __wt_mtx_destroy(toc->block)) != 0 && ret == 0)
		ret = tret;

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
	WT_TOC **q, **eq;
	u_int sid;

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
	if (toc->db == NULL || toc->db->idb->stoc == NULL)
		sid = WT_PSTOC_ID;
	else
		sid = toc->db->idb->stoc->id;
	stoc = WT_GLOBAL(sq) + (sid - 1);

	stoc->ops[toc->slot] = toc;

	(void)__wt_lock(toc->block);

	/* Return the operation's code. */
	return (toc->ret);
}
