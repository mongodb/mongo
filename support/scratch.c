/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_scr_alloc --
 *	Scratch buffer allocation function.
 */
int
__wt_scr_alloc(WT_TOC *toc, DBT **dbtp)
{
	DBT *scratch;
	ENV *env;
	u_int32_t allocated;
	u_int i;
	int ret;

	env = toc->env;

	/*
	 * There's an array of scratch buffers in each WT_TOC that can be used
	 * by any function.  We use DBTs for scratch buffers because we already
	 * have to have functions that do variable-length allocation on DBTs.
	 * Scratch buffers are allocated only by a single thread of control, so
	 * no locking is necessary.
	 */
	for (i = 0,
	    scratch = toc->scratch; i < toc->scratch_alloc; ++i, ++scratch)
		if (!F_ISSET(scratch, WT_SCRATCH_INUSE)) {
			*dbtp = scratch;
			F_SET(scratch, WT_SCRATCH_INUSE);
			return (0);
		}

	allocated = toc->scratch_alloc * sizeof(DBT);
	WT_ERR(__wt_realloc(env, &allocated,
	    (toc->scratch_alloc + 10) * sizeof(DBT), &toc->scratch));
	toc->scratch_alloc += 10;
	return (__wt_scr_alloc(toc, dbtp));

err:	__wt_api_env_errx(env,
	    "WT_TOC unable to allocate more scratch buffers");
	return (ret);
}

/*
 * __wt_scr_release --
 *	Release a scratch buffer.
 */
void
__wt_scr_release(DBT **dbt)
{
	DBT *scratch;

	scratch = *dbt;
	*dbt = NULL;

	F_CLR(scratch, WT_SCRATCH_INUSE);
}

/*
 * __wt_scr_free --
 *	Free all memory associated with the scratch buffers.
 */
void
__wt_scr_free(WT_TOC *toc)
{
	DBT *scratch;
	ENV *env;
	u_int i;

	env = toc->env;

	for (i = 0,
	    scratch = toc->scratch; i < toc->scratch_alloc; ++i, ++scratch)
		if (scratch->data != NULL)
			__wt_free(env, scratch->data, scratch->mem_size);

	__wt_free(env, toc->scratch, toc->scratch_alloc * sizeof(DBT));
}
