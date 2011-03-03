/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "bt_inline.c"

static int __wt_row_update(SESSION *, WT_ITEM *, WT_ITEM *, int);

/*
 * __wt_btree_row_del --
 *	Db.row_del method.
 */
int
__wt_btree_row_del(SESSION *session, WT_ITEM *key)
{
	return (__wt_row_update(session, key, NULL, 0));
}

/*
 * __wt_btree_row_put --
 *	Db.row_put method.
 */
int
__wt_btree_row_put(SESSION *session, WT_ITEM *key, WT_ITEM *data)
{
	return (__wt_row_update(session, key, data, 1));
}

/*
 * __wt_row_update --
 *	Row-store delete and update.
 */
static int
__wt_row_update(SESSION *session, WT_ITEM *key, WT_ITEM *data, int insert)
{
	WT_PAGE *page;
	WT_UPDATE **new_upd, *upd;
	int ret;

	new_upd = NULL;
	upd = NULL;

	/* Search the btree for the key. */
	WT_RET(__wt_row_search(session, key, insert ? WT_INSERT : 0));
	page = session->srch_page;

	/* Allocate an update array as necessary. */
	if (page->u.row_leaf.upd == NULL)
		WT_ERR(__wt_calloc_def(session, page->indx_count, &new_upd));

	/* Allocate room for the new data item from per-thread memory. */
	WT_ERR(__wt_update_alloc(session, &upd, data));

	/* Schedule the workQ to insert the WT_UPDATE structure. */
	__wt_item_update_serial(session, page, session->srch_write_gen,
	    WT_ROW_INDX_SLOT(page, session->srch_ip), new_upd, upd, ret);

	if (ret != 0) {
err:		if (upd != NULL)
			__wt_update_free(session, upd);
	}

	/* Free any update array unless the workQ used it. */
	if (new_upd != NULL && new_upd != page->u.row_leaf.upd)
		__wt_free(session, new_upd, page->indx_count * sizeof(WT_UPDATE *));

	WT_PAGE_OUT(session, page);

	return (0);
}

/*
 * __wt_item_update_serial_func --
 *	Server function to update a WT_UPDATE entry in the modification array.
 */
int
__wt_item_update_serial_func(SESSION *session)
{
	WT_PAGE *page;
	WT_UPDATE **new_upd, *upd;
	uint32_t slot, write_gen;
	int ret;

	__wt_item_update_unpack(session, page, write_gen, slot, new_upd, upd);

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

err:	__wt_session_serialize_wrapup(session, page, ret);
	return (0);
}

/*
 * __wt_update_alloc --
 *	Allocate a WT_UPDATE structure and associated data from the TOC's buffer
 *	and fill it in.
 */
int
__wt_update_alloc(SESSION *session, WT_UPDATE **updp, WT_ITEM *data)
{
	BTREE *btree;
	SESSION_BUFFER *sb;
	WT_UPDATE *upd;
	uint32_t align_size, alloc_size, size;
	int single_use;

	btree = session->btree;

	/*
	 * Allocate memory for a data insert or change; there's a buffer in the
	 * SESSION structure for allocation of chunks of memory to hold changed
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
	 * The first thing in each chunk of memory is a SESSION_BUFFER structure
	 * (which we check is a multiple of 4B during initialization); then one
	 * or more WT_UPDATE structure plus data chunk pairs.
	 *
	 * XXX
	 * Figure out how much space we need: this code limits the maximum size
	 * of a data item stored in the file.  In summary, for a big item we
	 * have to store a SESSION_BUFFER structure, the WT_UPDATE structure and
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
		return (__wt_file_item_too_big(btree));
	align_size = WT_ALIGN(size + sizeof(WT_UPDATE), sizeof(uint32_t));
	if (align_size > UINT32_MAX - sizeof(SESSION_BUFFER))
		return (__wt_file_item_too_big(btree));

	/*
	 * If we already have a buffer and the data fits, copy the WT_UPDATE
	 * structure and data into place, we're done.
	 */
	sb = session->sb;
	if (sb != NULL && align_size <= sb->space_avail)
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
	if (align_size > S2C(session)->data_update_max) {
		alloc_size = WT_SIZEOF32(SESSION_BUFFER) + align_size;
		single_use = 1;
	} else {
		alloc_size = __wt_nlpo2(
		    WT_MAX(align_size * 4, S2C(session)->data_update_initial));
		single_use = 0;
	}
	WT_RET(__wt_calloc(session, 1, alloc_size, &sb));

	sb->len = alloc_size;
	sb->space_avail = alloc_size - WT_SIZEOF32(SESSION_BUFFER);
	sb->first_free = (uint8_t *)sb + sizeof(SESSION_BUFFER);

	/*
	 * If it's a single use allocation, ignore any current SESSION buffer.
	 * Else, release the old SESSION buffer and replace it with the new one.
	 */
	if (!single_use) {
		/*
		 * The "in" reference count is artificially incremented by 1 as
		 * long as an SESSION buffer is referenced by the SESSION thread;
		 * we don't want them freed because a page was evicted and the
		 * count went to 0.  Decrement the reference count on the buffer
		 * as part of releasing it.  There's a similar reference count
		 * decrement when the SESSION structure is discarded.
		 *
		 * XXX
		 * There's a race here: if this code, or the SESSION structure
		 * close code, and the page discard code race, it's possible
		 * neither will realize the buffer is no longer needed and free
		 * it.  The fix is to involve the eviction or workQ threads:
		 * they may need a linked list of buffers they review to ensure
		 * it never happens.  I'm living with this now: it's unlikely
		 * and it's a memory leak if it ever happens.
		 */
		if (session->sb != NULL)
			--session->sb->in;
		session->sb = sb;

		sb->in = 1;
	}

no_allocation:
	/* Copy the WT_UPDATE structure into place. */
	upd = (WT_UPDATE *)sb->first_free;
	upd->sb = sb;
	if (data == NULL)
		WT_UPDATE_DELETED_SET(upd);
	else {
		upd->size = data->size;
		memcpy(WT_UPDATE_DATA(upd), data->data, data->size);
	}

	sb->first_free += align_size;
	sb->space_avail -= align_size;
	++sb->in;

	*updp = upd;
	return (0);
}

/*
 * __wt_update_free --
 *	Free a WT_UPDATE structure and associated data from the SESSION_BUFFER.
 */
void
__wt_update_free(SESSION *session, WT_UPDATE *upd)
{
	/*
	 * It's possible we allocated a WT_UPDATE structure and associated item
	 * memory from the SESSION buffer, but then an error occurred.  Don't
	 * try and clean up the SESSION buffer, it's simpler to decrement the
	 * use count and let the page discard code deal with it during the
	 * page reconciliation process.  (Note we're still in the allocation
	 * path, so we decrement the "in" field, not the "out" field.)
	 */
	--upd->sb->in;

	/*
	 * One other thing: if the SESSION buffer was a one-off, we have to free
	 * it here, it's not linked to any WT_PAGE in the system.
	 */
	if (upd->sb->in == 0)
		__wt_free(session, upd->sb, upd->sb->len);
}
