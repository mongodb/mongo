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

u_int __wt_cthread_count = 10;

/*
 * wt_env_toc_create --
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

	/* The mutex is self-blocking, so it's normal state is locked. */
	WT_ERR(__wt_calloc(env, 1, sizeof(WT_MTX), &toc->block));
	WT_ERR(__wt_mtx_init(toc->block));
	WT_ERR(__wt_lock(toc->block));

	/* Allocate a new WT_TOC slot. */
	WT_ERR(__wt_lock(&ienv->mtx));
	toc->srvr_slot = ienv->next_toc_srvr_slot++;
	WT_ERR(__wt_unlock(&ienv->mtx));
	if (toc->srvr_slot > __wt_cthread_count) {
		__wt_env_errx(env,
		    "Env->toc_create: too many threads: maximum %u",
		    __wt_cthread_count);
		ret = WT_ERROR;
		goto err;
	}

	WT_CLEAR(toc->key);
	WT_CLEAR(toc->data);

	/* WT_TOCs are enclosed by an environment. */
	toc->env = env;

	WT_TOC_CLEAR_SRVR(toc);

	toc->destroy = __wt_env_toc_destroy;

	if (F_ISSET(ienv, WT_SINGLE_THREADED))
		F_SET(toc, WT_SINGLE_THREADED);

	*tocp = toc;
	return (0);

err:	(void)__wt_env_toc_destroy(toc, 0);
	return (ret);
}

/*
 * __wt_env_toc_destroy --
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

	WT_FREE_AND_CLEAR(env, toc->key.data);
	WT_FREE_AND_CLEAR(env, toc->data.data);

	__wt_free(env, toc);

	return (ret);
}

/*
 * __wt_toc_sched --
 *	Schedule an operation.
 */
int
__wt_toc_sched(WT_TOC *toc)
{
	ENV *env;
	WT_SRVR *srvr;

	env = toc->env;

	/*
	 * Threads of control may re-enter the API.  Rather than re-implement
	 * the API, the WT_RUNNING flag will be set, which means we continue
	 * what we're doing, using the current thread of control, whatever that
	 * may be.
	 */
	if (F_ISSET(toc, WT_RUNNING)) {
		__wt_api_switch(toc);
		return (toc->ret);
	}

	WT_STAT_INCR(env->ienv->stats, TOTAL_OPS, "total operations");

	/*
	 * The whole engine may be single-threaded, in which case we use the
	 * primary server without further consideration.
	 */
	if (F_ISSET(toc, WT_SINGLE_THREADED)) {
		srvr = &env->ienv->psrvr;
		WT_TOC_SET_CACHE(toc, srvr);
		__wt_api_switch(toc);
		return (toc->ret);
	}

	/* Otherwise, select a server and run, until the operation finishes. */
	do {
		/* Select a server. */
		srvr = WT_SRVR_SELECT(toc);

		/* Queue the operation. */
		srvr->ops[toc->srvr_slot].toc = toc;
		WT_FLUSH_MEMORY;

		/* Wait for the operation to complete. */
		while (srvr->ops[toc->srvr_slot].toc != NULL)
			__wt_yield();
	} while (toc->ret == WT_RESCHEDULE);

	/* Reset the WT_TOC's server information. */
	WT_TOC_CLEAR_SRVR(toc);

	return (toc->ret);
}
