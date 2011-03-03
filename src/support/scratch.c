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
__wt_scr_alloc(SESSION *session, uint32_t size, WT_SCRATCH **scratchp)
{
	WT_SCRATCH *available, *scratch;
	uint32_t allocated;
	u_int i;
	int ret;

	/* Don't risk the caller not catching the error. */
	*scratchp = NULL;

	/*
	 * There's an array of scratch buffers in each SESSION that can be used
	 * by any function.  We use DBTs for scratch buffers because we already
	 * have to have functions that do variable-length allocation on DBTs.
	 * Scratch buffers are allocated only by a single thread of control, so
	 * no locking is necessary.
	 *
	 * Walk the list, looking for a buffer we can use.
	 */
	for (i = 0, available = NULL,
	    scratch = session->scratch; i < session->scratch_alloc; ++i, ++scratch)
		if (!F_ISSET(scratch, WT_SCRATCH_INUSE)) {
			if (size == 0 || scratch->mem_size >= size) {
				*scratchp = scratch;
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
		if (scratch->buf != NULL) {
			__wt_free(session, scratch->buf, scratch->mem_size);
			scratch->mem_size = 0;
		}
		WT_RET(
		    __wt_calloc(session, size, sizeof(uint8_t), &scratch->buf));
		scratch->mem_size = size;
		*scratchp = scratch;
		F_SET(scratch, WT_SCRATCH_INUSE);
		return (0);
	}

	/* Resize the array, we need more scratch buffers. */
	allocated = session->scratch_alloc * WT_SIZEOF32(WT_SCRATCH);
	WT_ERR(__wt_realloc(session, &allocated,
	    (session->scratch_alloc + 10) * sizeof(WT_SCRATCH), &session->scratch));
	session->scratch_alloc += 10;
	return (__wt_scr_alloc(session, size, scratchp));

err:	__wt_errx(session,
	    "SESSION unable to allocate more scratch buffers");
	return (ret);
}

/*
 * __wt_scr_release --
 *	Release a scratch buffer.
 */
void
__wt_scr_release(WT_SCRATCH **dbt)
{
	WT_SCRATCH *scratch;

	scratch = *dbt;
	*dbt = NULL;

	F_CLR(scratch, WT_SCRATCH_INUSE);
}

/*
 * __wt_scr_free --
 *	Free all memory associated with the scratch buffers.
 */
void
__wt_scr_free(SESSION *session)
{
	WT_SCRATCH *scratch;
	u_int i;

	for (i = 0,
	    scratch = session->scratch; i < session->scratch_alloc; ++i, ++scratch)
		if (scratch->item.data != NULL)
			__wt_free(session, scratch->item.data, scratch->mem_size);

	__wt_free(session, session->scratch, session->scratch_alloc * sizeof(WT_SCRATCH));
}
