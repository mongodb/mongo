/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_scr_alloc --
 *	Scratch buffer allocation function.
 */
int
__wt_scr_alloc(WT_TOC *toc, uint32_t size, DBT **dbtp)
{
	DBT *available, *scratch;
	ENV *env;
	uint32_t allocated;
	u_int i;
	int ret;

	env = toc->env;

	*dbtp = NULL;	/* Don't risk the caller not catching the error. */

	/*
	 * There's an array of scratch buffers in each WT_TOC that can be used
	 * by any function.  We use DBTs for scratch buffers because we already
	 * have to have functions that do variable-length allocation on DBTs.
	 * Scratch buffers are allocated only by a single thread of control, so
	 * no locking is necessary.
	 *
	 * Walk the list, looking for a buffer we can use.
	 */
	for (i = 0, available = NULL,
	    scratch = toc->scratch; i < toc->scratch_alloc; ++i, ++scratch)
		if (!F_ISSET(scratch, WT_SCRATCH_INUSE)) {
			if (size == 0 || scratch->mem_size >= size) {
				*dbtp = scratch;
				F_SET(scratch, WT_SCRATCH_INUSE);
				return (0);
			}
			available = scratch;
		}

	/*
	 * If available set, we found a slot but it wasn't large enough.
	 * Free any existing memory, and allocate something large enough.
	 */
	if (available != NULL) {
		scratch = available;
		if (scratch->data != NULL) {
			__wt_free(env, scratch->data, scratch->mem_size);
			scratch->data = NULL;
			scratch->mem_size = 0;
		}
		WT_RET(
		    __wt_calloc(env, size, sizeof(uint8_t), &scratch->data));
		scratch->mem_size = size;
		*dbtp = scratch;
		F_SET(scratch, WT_SCRATCH_INUSE);
		return (0);
	}

	/* Resize the array, we need more scratch buffers. */
	allocated = toc->scratch_alloc * WT_SIZEOF32(DBT);
	WT_ERR(__wt_realloc(env, &allocated,
	    (toc->scratch_alloc + 10) * sizeof(DBT), &toc->scratch));
	toc->scratch_alloc += 10;
	return (__wt_scr_alloc(toc, size, dbtp));

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
