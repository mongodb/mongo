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
 * __wt_update_alloc --
 *	Allocate space for a chunk of data in per-thread memory.
 */
int
__wt_update_alloc(WT_TOC *toc, u_int32_t size, void *retp)
{
	ENV *env;
	WT_DATA_UPDATE *upd;
	u_int32_t align_size, alloc_size;
	int single_use;

	env = toc->env;
	upd = toc->update;

	/*
	 * Each chunk is preceded by a pointer to the buffer's WT_DATA_UPDATE
	 * structure (so we can find it when we free this chunk of memory).
	 * Pad each chunk of memory so the pointer value is aligned correctly
	 * for the next chunk.
	 *
	 * XXX
	 * The aligned size is possibly greater than a 32-bit quantity, if we
	 * truly support 32-bit data items.  The solution is to drop back 32
	 * bytes from the maximum key/data item we support, no application is
	 * going to store 4GB in a single item (and if they do, it's a BLOB).
	 *
	 * XXX
	 * On a 64-bit machine, should align to an 8-byte boundary?
	 */
	align_size = WT_ALIGN(sizeof(WT_DATA_UPDATE *) + size, 4);

	/* If the data fits in the current buffer, we're done. */
	if (upd != NULL && align_size <= upd->space_avail)
		goto done;

	/*
	 * If the room we need is greater than the maximum data we'll group
	 * into WT_DATA_UPDATE buffers, allocate a single WT_DATA_UPDATE area.
	 */
	if (align_size > env->data_update_max) {
		alloc_size = align_size + sizeof(WT_DATA_UPDATE);
		single_use = 1;
		goto alloc;
	}

	/*
	 * Release any existing update buffer, and allocate a new one.
	 *
	 * We artificially increase the ref count on in-use buffers because we
	 * don't want them freed because a page was drained and their count
	 * went to 0.  Decrease the ref count on the existing buffer as part of
	 * releasing it.
	 *
	 * XXX
	 * There's a race here we have to fix: if the page discard thread and
	 * this thread race, then it's possible nobody will realize the buffer
	 * is no longer needed and free it.  I suspect the fix is to get the
	 * cache drain or workQ threads involved: they may need a linked list
	 * of buffers they review to ensure this never happens.  I'm living
	 * with this now: it's pretty unlikely and it's a memory leak if it
	 * ever happens.
	 */
	if (upd != NULL) {
		--upd->in;
		toc->update = NULL;
	}

	/*
	 * Allocate the next power-of-two larger than 4 times the requested
	 * size, and at least the default buffer size.
	 *
	 * XXX
	 * Why allocate 4x the requested size?
	 */
	alloc_size = __wt_nlpo2(WT_MAX(size * 4, env->data_update_initial));
	single_use = 0;

alloc:	WT_RET(__wt_calloc(env, 1, alloc_size, &upd));
	if (!single_use)
		toc->update = upd;
	upd->len = alloc_size;
	upd->first_free = (u_int8_t *)upd + sizeof(WT_DATA_UPDATE);
	upd->space_avail = alloc_size - sizeof(WT_DATA_UPDATE);

done:	*(WT_DATA_UPDATE **)upd->first_free = upd;
	*(void **)retp = upd->first_free + sizeof(WT_DATA_UPDATE *);
	upd->first_free += align_size;
	upd->space_avail -= align_size;
	++upd->in;
	return (0);
}
