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
 * __wt_workq_flist --
 *	Called when the first entry on the flist queue can be freed.
 */
void
__wt_workq_flist(ENV *env)
{
	IENV *ienv;
	WT_FLIST *fp;
	u_int cnt;

	ienv = env->ienv;

	fp = TAILQ_FIRST(&ienv->flistq);
	TAILQ_REMOVE(&ienv->flistq, fp, q);

	for (cnt = 0; cnt < fp->cnt; ++cnt)
		__wt_free(env, fp->ref[cnt].p, fp->ref[cnt].len);
	__wt_free(env, fp, sizeof(WT_FLIST));
}

/*
 * __wt_flist_insert --
 *	Add a memory reference to our to-be-free'd list.
 */
void
__wt_flist_insert(WT_TOC *toc, void *p, u_int32_t len)
{
	ENV *env;
	WT_FLIST *fp;

	env = toc->env;

	/*
	 * Allocate a new chunk; any existing chunk is handed off to the workQ
	 * thread to be free'd later.
	 */
	if (toc->flist != NULL) {
		(void)__wt_flist_sched(toc);
		toc->flist = NULL;
	}
	if (__wt_calloc(env, 1, sizeof(WT_FLIST), &fp) != 0)
		return;

	fp->ref[0].p = p;
	fp->ref[0].len = len;
	fp->cnt = 1;
	toc->flist = fp;
}

/*
 * __wt_flist_sched --
 *	Hand a WT_FLIST structure to the workQ, the WT_TOC is finished with it.
 */
int
__wt_flist_sched(WT_TOC *toc)
{
	IENV *ienv;
	WT_FLIST *fp;
	int ret;

	ienv = toc->env->ienv;
	fp = toc->flist;

	/*
	 * Memory references in this list can be freed when threads currently
	 * in the library have exited.
	 */
	fp->gen = ienv->api_gen + 1;

	__wt_flist_free_serial(toc, fp, ret);

	return (ret);
}

/*
 * __wt_flist_free_serial_func --
 *	Add a WT_FLIST structure to the workQ's list.
 */
int
__wt_flist_free_serial_func(WT_TOC *toc)
{
	IENV *ienv;
	WT_FLIST *fp;

	__wt_flist_free_unpack(toc, fp);
	ienv = toc->env->ienv;

	TAILQ_INSERT_TAIL(&ienv->flistq, fp, q);
	return (0);
}
