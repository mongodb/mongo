/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_row_update(WT_TOC *, DBT *, DBT *, int);

/*
 * __wt_db_row_del --
 *	Db.row_del method.
 */
inline int
__wt_db_row_del(WT_TOC *toc, DBT *key)
{
	return (__wt_bt_row_update(toc, key, NULL, 0));
}

/*
 * __wt_db_row_put --
 *	Db.row_put method.
 */
inline int
__wt_db_row_put(WT_TOC *toc, DBT *key, DBT *data)
{
	return (__wt_bt_row_update(toc, key, data, 1));
}

/*
 * __wt_bt_row_update --
 *	Row store delete and update.
 */
static int
__wt_bt_row_update(WT_TOC *toc, DBT *key, DBT *data, int insert)
{
	ENV *env;
	IDB *idb;
	WT_PAGE *page;
	WT_REPL **new_repl, *repl;
	int ret;

	env = toc->env;
	idb = toc->db->idb;
	new_repl = NULL;
	repl = NULL;

	/* Search the btree for the key. */
	WT_RET(__wt_bt_search_row(toc, key, insert ? WT_INSERT : 0));
	page = toc->srch_page;

	/* Allocate a page replacement array as necessary. */
	if (page->repl == NULL)
		WT_ERR(__wt_calloc(
		    env, page->indx_count, sizeof(WT_REPL *), &new_repl));

	/* Allocate room for the new data item from pre-thread memory. */
	WT_ERR(__wt_bt_repl_alloc(toc, &repl, data));

	/* Schedule the workQ to insert the WT_REPL structure. */
	__wt_bt_update_serial(toc, page, toc->srch_write_gen,
	    WT_ROW_SLOT(page, toc->srch_ip), new_repl, repl, ret);

	if (ret != 0) {
err:		if (repl != NULL)
			__wt_bt_repl_free(toc, repl);
	}

	/* Free any replacement array unless the workQ used it. */
	if (new_repl != NULL && new_repl != page->repl)
		__wt_free(env, new_repl, page->indx_count * sizeof(WT_REPL *));

	if (page != NULL && page != idb->root_page)
		__wt_bt_page_out(toc, &page, ret == 0 ? WT_MODIFIED : 0);

	return (0);
}

/*
 * __wt_bt_update_serial_func --
 *	Server function to update a WT_REPL entry in the modification array.
 */
int
__wt_bt_update_serial_func(WT_TOC *toc)
{
	WT_PAGE *page;
	WT_REPL **new_repl, *repl;
	uint16_t write_gen;
	int ret, slot;

	__wt_bt_update_unpack(toc, page, write_gen, slot, new_repl, repl);

	ret = 0;

	/* Check the page's write-generation, then update it. */
	WT_ERR(__wt_page_write_gen_update(page, write_gen));

	/*
	 * If the page does not yet have a replacement array, our caller passed
	 * us one of the correct size.   (It's the caller's responsibility to
	 * detect & free the passed-in expansion array if we don't use it.)
	 */
	if (page->repl == NULL)
		page->repl = new_repl;

	/*
	 * Insert the new WT_REPL as the first item in the forward-linked list
	 * of replacement structures.  Flush memory to ensure the list is never
	 * broken.
	 */
	repl->next = page->repl[slot];
	WT_MEMORY_FLUSH;
	page->repl[slot] = repl;
	WT_PAGE_MODIFY_SET(page);
	/*
	 * Depend on the memory flush in __wt_toc_serialize_wrapup before the
	 * calling thread proceeds.
	 */

err:	__wt_toc_serialize_wrapup(toc, ret);
	return (0);
}

/*
 * __wt_bt_repl_alloc --
 *	Allocate a WT_REPL structure and associated data from the TOC's update
 *	memory, and fill it in.
 */
int
__wt_bt_repl_alloc(WT_TOC *toc, WT_REPL **replp, DBT *data)
{
	DB *db;
	ENV *env;
	WT_REPL *repl;
	WT_TOC_UPDATE *update;
	uint32_t align_size, alloc_size, size;
	int single_use;

	env = toc->env;
	db = toc->db;

	/*
	 * Allocate memory for a data insert or change; there's a buffer in the
	 * WT_TOC structure for allocation of chunks of memory to hold changed
	 * or inserted data items.
	 *
	 * We align each allocation because we directly access WT_REPL structure
	 * fields in the memory (the x86 handles unaligned accesses, but I don't
	 * want to have to find and fix this code for a port to a system that
	 * doesn't handle unaligned accesses).  It wastes space, but this memory
	 * is never written to disk and there are fewer concerns about memory
	 * than with on-disk structures.  Any other code allocating memory from
	 * this buffer needs to align its allocations as well.
	 *
	 * The first thing in each chunk of memory is WT_TOC_UPDATE structure
	 * (which we check is a multiple of 4B during initialization); then
	 * there are one or more WT_REPL structure plus data chunk pairs.
	 *
	 * XXX
	 * Figure out how much space we need: this code limits the maximum size
	 * of a data item stored in the database.  In summary, for a big item we
	 * have to store a WT_TOC_UPDATE structure, the WT_REPL structure and
	 * the data, all in an allocated buffer.   We only pass a 32-bit value
	 * to our allocation routine, so we can't store an item bigger than the
	 * maximum 32-bit value minus the sizes of those two structures, where
	 * the WT_REPL structure and data item are aligned to a 32-bit boundary.
	 * We could fix this, but it's unclear it's worth the effort -- document
	 * you can store a (4GB - 20B) item max, and you're done, because it's
	 * insane to store a 4GB item in the database anyway.
	 *
	 * Check first we won't overflow when calculating an aligned size, then
	 * check the total required space for this item.
	 */
	size = data == NULL ? 0 : data->size;
	if (UINT32_MAX - size < sizeof(WT_REPL) + sizeof(uint32_t))
		return (__wt_database_item_too_big(db));
	align_size = WT_ALIGN(size + sizeof(WT_REPL), sizeof(uint32_t));
	if (UINT32_MAX - align_size < sizeof(WT_TOC_UPDATE))
		return (__wt_database_item_too_big(db));

	/*
	 * If we already have a buffer and the data fits, just copy the WT_REPL
	 * structure and data into place, we're done.
	 */
	update = toc->update;
	if (update != NULL && align_size <= update->space_avail)
		goto no_allocation;

	/*
	 * Decide how much memory to allocate: if it's a one-off (that is, the
	 * data is bigger than anything we'll aggregate into these buffers, it's
	 * a one-off.  Otherwise, allocate the next power-of-two larger than 4
	 * times the requested size, and at least the default buffer size.
	 *
	 * XXX
	 * I have no reason for the 4x the request size, I just hate to allocate
	 * a buffer for every change to the database.  A better approach would
	 * be to grow the allocation buffer as the thread makes more changes; if
	 * a thread is doing lots of work, give it lots of memory, otherwise
	 * only allocate as it's necessary.
	 */
	if (align_size > env->data_update_max) {
		alloc_size = sizeof(WT_TOC_UPDATE) + align_size;
		single_use = 1;
	} else {
		alloc_size = __wt_nlpo2(
		    WT_MAX(align_size * 4, env->data_update_initial));
		single_use = 0;
	}
	WT_RET(__wt_calloc(env, 1, alloc_size, &update));

	update->len = alloc_size;
	update->space_avail = alloc_size - sizeof(WT_TOC_UPDATE);
	update->first_free = (uint8_t *)update + sizeof(WT_TOC_UPDATE);

	/*
	 * If it's a single use allocation, ignore any current update buffer.
	 * Else, release the old update buffer and replace it with the new one.
	 */
	if (!single_use) {
		/*
		 * The "in" reference count is artificially incremented by 1 as
		 * long as an update buffer is referenced by the WT_TOC thread;
		 * we don't want them freed because a page was drained and their
		 * count went to 0.  Decrement the reference count on the buffer
		 * as part of releasing it.  There's a similar reference count
		 * decrement when the WT_TOC structure is discarded.
		 *
		 * XXX
		 * There's a race here: if this code, or the WT_TOC structure
		 * close code, and the page discard code race, it's possible
		 * neither will realize the buffer is no longer needed and free
		 * it.  The fix is to involve the cache drain or workQ threads:
		 * they may need a linked list of buffers they review to ensure
		 * it never happens.  I'm living with this now: it's unlikely
		 * and it's a memory leak if it ever happens.
		 */
		if (toc->update != NULL)
			--toc->update->in;
		toc->update = update;

		update->in = 1;
	}


no_allocation:
	/* Copy the WT_REPL structure into place. */
	repl = (WT_REPL *)update->first_free;
	repl->update = update;
	if (data == NULL)
		WT_REPL_DELETED_SET(repl);
	else {
		repl->size = data->size;
		memcpy(WT_REPL_DATA(repl), data->data, data->size);
	}

	update->first_free += align_size;
	update->space_avail -= align_size;
	++update->in;

	*replp = repl;
	return (0);
}

/*
 * __wt_bt_repl_free --
 *	Free a WT_REPL structure and associated data from the TOC's update
 *	memory.
 */
void
__wt_bt_repl_free(WT_TOC *toc, WT_REPL *repl)
{
	ENV *env;

	env = toc->env;

	/*
	 * It's possible we allocated a WT_REPL structure and associated item
	 * memory from the WT_TOC update buffer, but then an error occurred.
	 * Don't try and clean up the update buffer, it's simpler to decrement
	 * the use count and let the page discard code deal with it during the
	 * page reconciliation process.  (Note we're still in the allocation
	 * path, so we decrement the "in" field, not the "out" field.)
	 */
	--repl->update->in;

	/*
	 * One other thing: if the update buffer was a one-off, we have to free
	 * it here, it's not linked to any WT_PAGE in the system.
	 */
	if (repl->update->in == 0)
		__wt_free(env, repl->update, repl->update->len);
}
