/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_toc_destroy(WT_TOC *, u_int32_t);

/*
 * wt_toc_create --
 *	WT_TOC constructor.
 */
int
wt_toc_create(WT_TOC **tocp, u_int32_t flags)
{
	WT_TOC *toc;
	int ret;

	if ((ret = __wt_calloc(NULL, 1, sizeof(WT_TOC), &toc)) != 0)
		return (ret);
	if ((ret = __wt_calloc(NULL, 1, sizeof(WT_MTX), &toc->mtx)) != 0)
		goto err;

	/* The mutex is self-blocking, so it's normal state is locked. */
	if ((ret = __wt_mtx_init(toc->mtx)) != 0)
		goto err;
	if ((ret = __wt_lock(toc->mtx)) != 0)
		goto err;

	toc->slot = WT_GLOBAL(workq_next)++;
	toc->destroy = __wt_toc_destroy;

	*tocp = toc;
	return (0);

err:	(void)__wt_toc_destroy(toc, 0);
	return (ret);
}

/*
 * __wt_toc_destroy --
 *	toc.destroy method (WT_TOC destructor).
 */
static int
__wt_toc_destroy(WT_TOC *toc, u_int32_t flags)
{
	WT_FREE_AND_CLEAR(NULL, toc->mtx);

	__wt_free(NULL, toc);

	return (0);
}

/*
 * __wt_toc_sched --
 *	Schedule an operation.
 */
int
__wt_toc_sched(WT_TOC *toc)
{
	WT_TOC **q;

	/* Don't schedule any operations if there's no queue. */
	if (WT_GLOBAL(workq) == NULL)
		return (WT_ERROR);

	/*
	 * Threads of control sometimes re-enter the API.  Rather than code
	 * up lots of internal versions of the routine, allow calls through
	 * the standard API layer.
	 */
	if (F_ISSET(toc, WT_RUNNING))
		return (__wt_api_switch(toc));

	/* Otherwise, schedule the call and go to sleep until it completes. */
	q = WT_GLOBAL(workq) + toc->slot;
	*q = toc;
	WT_FLUSH_MEMORY;

	(void)__wt_lock(toc->mtx);

	return (toc->ret);
}
