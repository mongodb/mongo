/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "bt_inline.c"

static int __wt_row_update(WT_TOC *, WT_DATAITEM *, WT_DATAITEM *, int);

/*
 * __wt_db_row_del --
 *	Db.row_del method.
 */
int
__wt_db_row_del(WT_TOC *toc, WT_DATAITEM *key)
{
	return (__wt_row_update(toc, key, NULL, 0));
}

/*
 * __wt_db_row_put --
 *	Db.row_put method.
 */
int
__wt_db_row_put(WT_TOC *toc, WT_DATAITEM *key, WT_DATAITEM *data)
{
	return (__wt_row_update(toc, key, data, 1));
}

/*
 * __wt_row_update --
 *	Row-store delete and update.
 */
static int
__wt_row_update(WT_TOC *toc, WT_DATAITEM *key, WT_DATAITEM *data, int insert)
{
	ENV *env;
	WT_PAGE *page;
	WT_UPDATE **new_upd, *upd;
	int ret;

	env = toc->env;
	new_upd = NULL;
	upd = NULL;

	/* Search the btree for the key. */
	WT_RET(__wt_row_search(toc, key, insert ? WT_INSERT : 0));
	page = toc->srch_page;

	/* Allocate an update array as necessary. */
	if (page->u.row_leaf.upd == NULL)
		WT_ERR(__wt_calloc_def(env, page->indx_count, &new_upd));

	/* Allocate room for the new data item from per-thread memory. */
	WT_ERR(__wt_update_alloc(toc, &upd, data));

	/* Schedule the workQ to insert the WT_UPDATE structure. */
	__wt_item_update_serial(toc, page, toc->srch_write_gen,
	    WT_ROW_INDX_SLOT(page, toc->srch_ip), new_upd, upd, ret);

	if (ret != 0) {
err:		if (upd != NULL)
			__wt_update_free(toc, upd);
	}

	/* Free any update array unless the workQ used it. */
	if (new_upd != NULL && new_upd != page->u.row_leaf.upd)
		__wt_free(env, new_upd, page->indx_count * sizeof(WT_UPDATE *));

	WT_PAGE_OUT(toc, page);

	return (0);
}

/*
 * __wt_item_update_serial_func --
 *	Server function to update a WT_UPDATE entry in the modification array.
 */
int
__wt_item_update_serial_func(WT_TOC *toc)
{
	WT_PAGE *page;
	WT_UPDATE **new_upd, *upd;
	uint32_t slot, write_gen;
	int ret;

	__wt_item_update_unpack(toc, page, write_gen, slot, new_upd, upd);

	ret = 0;

	/* Check the page's write-generation. */
	WT_ERR(__wt_page_write_gen_check(page, write_gen));

	/*
	 * If the page does not yet have an update array, our caller passed
	 * us one of the correct size.   (It's the caller's responsibility to
	 * detect & free the passed-in expansion array if we don't use it.)
	 */
	if (page->u.row_leaf.upd == NULL)
		page->u.row_leaf.upd = new_upd;

	/*
	 * Insert the new WT_UPDATE as the first item in the forward-linked list
	 * of updates, flush memory to ensure the list is never broken.
	 */
	upd->next = page->u.row_leaf.upd[slot];
	WT_MEMORY_FLUSH;
	page->u.row_leaf.upd[slot] = upd;

err:	__wt_toc_serialize_wrapup(toc, page, ret);
	return (0);
}

/*
 * __wt_update_alloc --
 *	Allocate a WT_UPDATE structure and associated data from the TOC's buffer
 *	and fill it in.
 */
int
__wt_update_alloc(WT_TOC *toc, WT_UPDATE **updp, WT_DATAITEM *data)
{
	DB *db;
	ENV *env;
	WT_TOC_BUFFER *tb;
	WT_UPDATE *upd;
	uint32_t align_size, alloc_size, size;
	int single_use;

	env = toc->env;
	db = toc->db;

	/*
	 * Allocate memory for a data insert or change; there's a buffer in the
	 * WT_TOC structure for allocation of chunks of memory to hold changed
	 * or inserted data items.
	 *
	 * We align allocations because we directly access WT_UPDATE structure
	 * fields in the memory (the x86 handles unaligned accesses, but I don't
	 * want to have to find and fix this code for a port to a system that
	 * doesn't handle unaligned accesses).  It wastes space, but this memory
	 * is never written to disk and there are fewer concerns about memory
	 * than with on-disk structures.  Any other code allocating memory from
	 * this buffer needs to align its allocations as well.
	 *
	 * The first thing in each chunk of memory is a WT_TOC_BUFFER structure
	 * (which we check is a multiple of 4B during initialization); then one
	 * or more WT_UPDATE structure plus data chunk pairs.
	 *
	 * XXX
	 * Figure out how much space we need: this code limits the maximum size
	 * of a data item stored in the file.  In summary, for a big item we
	 * have to store a WT_TOC_BUFFER structure, the WT_UPDATE structure and
	 * the data, all in an allocated buffer.   We only pass a 32-bit value
	 * to our allocation routine, so we can't store an item bigger than the
	 * maximum 32-bit value minus the sizes of those two structures, where
	 * the WT_UPDATE structure and data item are aligned to a 32-bit
	 * boundary.  We could fix this, but it's unclear it's worth the effort
	 * -- document you can store a (4GB - 20B) item max, and you're done,
	 * because it's insane to store a 4GB item in the file anyway.
	 *
	 * Check first we won't overflow when calculating an aligned size, then
	 * check the total required space for this item.
	 */
	size = data == NULL ? 0 : data->size;
	if (size > UINT32_MAX - (sizeof(WT_UPDATE) + sizeof(uint32_t)))
		return (__wt_file_item_too_big(db));
	align_size = WT_ALIGN(size + sizeof(WT_UPDATE), sizeof(uint32_t));
	if (align_size > UINT32_MAX - sizeof(WT_TOC_BUFFER))
		return (__wt_file_item_too_big(db));

	/*
	 * If we already have a buffer and the data fits, copy the WT_UPDATE
	 * structure and data into place, we're done.
	 */
	tb = toc->tb;
	if (tb != NULL && align_size <= tb->space_avail)
		goto no_allocation;

	/*
	 * Decide how much memory to allocate: if it's a one-off (that is, the
	 * data is bigger than anything we'll aggregate into these buffers, it's
	 * a one-off.  Otherwise, allocate the next power-of-two larger than 4
	 * times the requested size, and at least the default buffer size.
	 *
	 * XXX
	 * I have no reason for the 4x the request size, I just hate to allocate
	 * a buffer for every change to the file.  A better approach would be to
	 * grow the allocation buffer as the thread makes more changes; if a
	 * thread is doing lots of work, give it lots of memory, otherwise only
	 * allocate as it's necessary.
	 */
	if (align_size > env->data_update_max) {
		alloc_size = WT_SIZEOF32(WT_TOC_BUFFER) + align_size;
		single_use = 1;
	} else {
		alloc_size = __wt_nlpo2(
		    WT_MAX(align_size * 4, env->data_update_initial));
		single_use = 0;
	}
	WT_RET(__wt_calloc(env, 1, alloc_size, &tb));

	tb->len = alloc_size;
	tb->space_avail = alloc_size - WT_SIZEOF32(WT_TOC_BUFFER);
	tb->first_free = (uint8_t *)tb + sizeof(WT_TOC_BUFFER);

	/*
	 * If it's a single use allocation, ignore any current WT_TOC buffer.
	 * Else, release the old WT_TOC buffer and replace it with the new one.
	 */
	if (!single_use) {
		/*
		 * The "in" reference count is artificially incremented by 1 as
		 * long as an WT_TOC buffer is referenced by the WT_TOC thread;
		 * we don't want them freed because a page was evicted and the
		 * count went to 0.  Decrement the reference count on the buffer
		 * as part of releasing it.  There's a similar reference count
		 * decrement when the WT_TOC structure is discarded.
		 *
		 * XXX
		 * There's a race here: if this code, or the WT_TOC structure
		 * close code, and the page discard code race, it's possible
		 * neither will realize the buffer is no longer needed and free
		 * it.  The fix is to involve the eviction or workQ threads:
		 * they may need a linked list of buffers they review to ensure
		 * it never happens.  I'm living with this now: it's unlikely
		 * and it's a memory leak if it ever happens.
		 */
		if (toc->tb != NULL)
			--toc->tb->in;
		toc->tb = tb;

		tb->in = 1;
	}

no_allocation:
	/* Copy the WT_UPDATE structure into place. */
	upd = (WT_UPDATE *)tb->first_free;
	upd->tb = tb;
	if (data == NULL)
		WT_UPDATE_DELETED_SET(upd);
	else {
		upd->size = data->size;
		memcpy(WT_UPDATE_DATA(upd), data->data, data->size);
	}

	tb->first_free += align_size;
	tb->space_avail -= align_size;
	++tb->in;

	*updp = upd;
	return (0);
}

/*
 * __wt_update_free --
 *	Free a WT_UPDATE structure and associated data from the WT_TOC_BUFFER.
 */
void
__wt_update_free(WT_TOC *toc, WT_UPDATE *upd)
{
	ENV *env;

	env = toc->env;

	/*
	 * It's possible we allocated a WT_UPDATE structure and associated item
	 * memory from the WT_TOC buffer, but then an error occurred.  Don't
	 * try and clean up the WT_TOC buffer, it's simpler to decrement the
	 * use count and let the page discard code deal with it during the
	 * page reconciliation process.  (Note we're still in the allocation
	 * path, so we decrement the "in" field, not the "out" field.)
	 */
	--upd->tb->in;

	/*
	 * One other thing: if the WT_TOC buffer was a one-off, we have to free
	 * it here, it's not linked to any WT_PAGE in the system.
	 */
	if (upd->tb->in == 0)
		__wt_free(env, upd->tb, upd->tb->len);
}
