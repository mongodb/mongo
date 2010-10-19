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
 * __wt_toc_scratch_alloc --
 *	Get a scratch buffer.
 */
int
__wt_toc_scratch_alloc(WT_TOC *toc, DBT **dbtp)
{
	ENV *env;
	u_int i;

	env = toc->env;

	/*
	 * There's an array of scratch buffers in each WT_TOC that can be used
	 * by any function.  We use DBTs for scratch buffers because we already
	 * have to have functions that do variable-length allocation on DBTs.
	 * Scratch buffers are allocated only by a single thread of control, so
	 * no locking is necessary.
	 */
	for (i = 0; i < WT_ELEMENTS(toc->scratch); ++i)
		if (toc->scratch_inuse[i] == 0) {
			*dbtp = &toc->scratch[i];
			toc->scratch_inuse[i] = 1;
			return (0);
		}

	__wt_api_env_errx(env, "WT_TOC has no more scratch buffer to allocate");
	return (WT_ERROR);
}

/*
 * __wt_toc_scratch_discard --
 *	Discard a scratch buffer.
 */
void
__wt_toc_scratch_discard(WT_TOC *toc, DBT *dbt)
{
	toc->scratch_inuse[dbt - &toc->scratch[0]] = 0;
}

/*
 * __wt_toc_scratch_free --
 *	Free all memory associated with the scratch buffers.
 */
void
__wt_toc_scratch_free(WT_TOC *toc)
{
	ENV *env;
	u_int i;

	env = toc->env;

	for (i = 0; i < WT_ELEMENTS(toc->scratch); ++i) {
		if (toc->scratch_inuse[i] != 0)
			__wt_api_env_errx(env,
			    "WT_TOC scratch buffers allocated but never "
			    "discarded");
		if (toc->scratch[i].data != NULL)
			__wt_free(env,
			    toc->scratch[i].data, toc->scratch[i].mem_size);
	}
}
