/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static void __wt_toc_link_op(ENV *, WT_TOC *, int);

/*
 * __wt_api_env_toc --
 *	WT_TOC constructor.
 */
int
__wt_api_env_toc(ENV *env, u_int32_t flags, WT_TOC **tocp)
{
	WT_TOC *toc;
	IENV *ienv;

	ienv = env->ienv;
	*tocp = NULL;

	WT_ENV_FCHK(env, "Env.toc", flags, WT_APIMASK_ENV_TOC);

	WT_RET(__wt_calloc(env, 1, sizeof(WT_TOC), &toc));
	toc->env = env;

	/* Initialize the methods. */
	__wt_methods_wt_toc_lockout(toc);
	__wt_methods_wt_toc_init_transition(toc);

	if (F_ISSET(ienv, WT_SINGLE_THREADED))
		F_SET(toc, WT_SINGLE_THREADED);

	__wt_toc_link_op(env, toc, 1);		/* Add to the ENV's list */

	*tocp = toc;
	return (0);
}

/*
 * __wt_api_wt_toc_close --
 *	toc.close method (WT_TOC close + destructor).
 */
int
__wt_api_wt_toc_close(WT_TOC *toc, u_int32_t flags)
{
	ENV *env;
	int ret;

	env = toc->env;
	ret = 0;

	WT_ENV_FCHK_NOTFATAL(
	    env, "WtToc.close", flags, WT_APIMASK_WT_TOC_CLOSE, ret);

	/*
	 * No matter what, this handle is dead -- make sure the structure is
	 * ignored by the workQ.
	 */
	F_SET(toc, WT_INVALID);
	WT_FLUSH_MEMORY;

	WT_FREE_AND_CLEAR(env, toc->key.data);
	WT_FREE_AND_CLEAR(env, toc->data.data);

	__wt_toc_link_op(env, toc, 0);		/* Delete from the ENV's list */

	return (ret);
}

/*
 * __wt_toc_link_op --
 *	Add/delete the WT_TOC to/from the environment's list of WT_TOCs.
 */
static void
__wt_toc_link_op(ENV *env, WT_TOC *toc, int add_op)
{
	IENV *ienv;

	ienv = env->ienv;

	/*
	 * This is tricky in the non-single-threaded case because the primary
	 * thread is walking the WT_TOC queue without acquiring any kind of
	 * lock.  The way we do it is to acquire the IENV mutex and set the
	 * toc_add/toc_del field, then return, leaving the IENV locked.   At
	 * some point, the workQ thread wakes up, adds/deletes the toc to/from
	 * the queue, clears the toc_add/toc_del field, and releases the mutex.
	 */
	__wt_lock(env, &ienv->mtx);
	if (F_ISSET(ienv, WT_SINGLE_THREADED)) {
		if (add_op)
			TAILQ_INSERT_TAIL(&ienv->tocqh, toc, q);
		else {
			TAILQ_REMOVE(&ienv->tocqh, toc, q);
			__wt_free(env, toc);
		}

		__wt_unlock(&ienv->mtx);
	} else {
		if (add_op)
			ienv->toc_add = toc;
		else
			ienv->toc_del = toc;
		WT_FLUSH_MEMORY;
	}
}
