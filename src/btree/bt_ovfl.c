/*-
 * Copyright (c) 2014-2017 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __ovfl_read --
 *	Read an overflow item from the disk.
 */
static int
__ovfl_read(WT_SESSION_IMPL *session,
    const uint8_t *addr, size_t addr_size, WT_ITEM *store)
{
	WT_BTREE *btree;
	const WT_PAGE_HEADER *dsk;

	btree = S2BT(session);

	/*
	 * Read the overflow item from the block manager, then reference the
	 * start of the data and set the data's length.
	 *
	 * Overflow reads are synchronous. That may bite me at some point, but
	 * WiredTiger supports large page sizes, overflow items should be rare.
	 */
	WT_RET(__wt_bt_read(session, store, addr, addr_size));
	dsk = store->data;
	store->data = WT_PAGE_HEADER_BYTE(btree, dsk);
	store->size = dsk->u.datalen;

	WT_STAT_CONN_INCR(session, cache_read_overflow);
	WT_STAT_DATA_INCR(session, cache_read_overflow);

	return (0);
}

/*
 * __wt_ovfl_read --
 *	Bring an overflow item into memory.
 */
int
__wt_ovfl_read(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_CELL_UNPACK *unpack, WT_ITEM *store, bool *decoded)
{
	WT_DECL_RET;
	WT_OVFL_TRACK *track;
	WT_UPDATE *upd;
	size_t i;

	*decoded = false;

	/*
	 * If no page specified, there's no need to lock and there's no cache
	 * to search, we don't care about WT_CELL_VALUE_OVFL_RM cells.
	 */
	if (page == NULL)
		return (
		    __ovfl_read(session, unpack->data, unpack->size, store));

	/*
	 * WT_CELL_VALUE_OVFL_RM cells: If reconciliation deleted an overflow
	 * value, but there was still a reader in the system that might need it,
	 * the on-page cell type will have been reset to WT_CELL_VALUE_OVFL_RM
	 * and we will be passed a page so we can check the on-page cell.
	 *
	 * Acquire the overflow lock, and retest the on-page cell's value inside
	 * the lock.
	 */
	__wt_readlock(session, &S2BT(session)->ovfl_lock);
	if (__wt_cell_type_raw(unpack->cell) == WT_CELL_VALUE_OVFL_RM) {
		track = page->modify->ovfl_track;
		for (upd = NULL, i = 0; i < track->remove_next; ++i)
			if (track->remove[i].cell == unpack->cell) {
				upd = track->remove[i].upd;
				break;
			}
		WT_ASSERT(session, i < track->remove_next);
		store->data = upd->data;
		store->size = upd->size;
		*decoded = true;
	} else
		ret = __ovfl_read(session, unpack->data, unpack->size, store);
	__wt_readunlock(session, &S2BT(session)->ovfl_lock);

	return (ret);
}

/*
 * __ovfl_cache_col_visible --
 *	column-store: check for a globally visible update.
 */
static bool
__ovfl_cache_col_visible(
    WT_SESSION_IMPL *session, WT_UPDATE *upd, WT_CELL_UNPACK *unpack)
{
	/*
	 * Column-store is harder than row_store: we're here because there's a
	 * reader in the system that might read the original version of an
	 * overflow record, which might match a number of records.  For example,
	 * the original overflow value was for records 100-200, we've replaced
	 * each of those records individually, but there exists a reader that
	 * might read any one of those records, and all of those records have
	 * different update entries with different transaction IDs.  Since it's
	 * infeasible to determine if there's a globally visible update for each
	 * reader for each record, we test the simple case where a single record
	 * has a single, globally visible update.  If that's not the case, cache
	 * the value.
	 */
	if (__wt_cell_rle(unpack) == 1 &&
	    WT_UPDATE_DATA_VALUE(upd) && __wt_txn_upd_visible_all(session, upd))
		return (true);
	return (false);
}

/*
 * __ovfl_cache_row_visible --
 *	row-store: check for a globally visible update.
 */
static bool
__ovfl_cache_row_visible(WT_SESSION_IMPL *session, WT_UPDATE *upd)
{
	/* Check to see if there's a globally visible update. */
	for (; upd != NULL; upd = upd->next)
		 if (WT_UPDATE_DATA_VALUE(upd) &&
		     __wt_txn_upd_visible_all(session, upd))
			return (true);

	return (false);
}

/*
 * __ovfl_cache_append_update --
 *	Append an overflow value to the update list.
 */
static int
__ovfl_cache_append_update(WT_SESSION_IMPL *session, WT_PAGE *page,
    WT_UPDATE *upd_list, WT_CELL_UNPACK *unpack, WT_UPDATE **updp)
{
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_UPDATE *append, *upd;
	size_t size;

	*updp = NULL;

	/* Read the overflow value. */
	WT_RET(__wt_scr_alloc(session, 1024, &tmp));
	WT_ERR(__wt_dsk_cell_data_ref(session, page->type, unpack, tmp));

	/*
	 * Create an update entry with no transaction ID to ensure global
	 * visibility, append it to the update list.
	 *
	 * We don't need locks or barriers in this function: any thread reading
	 * the update list will see our newly appended record or not, it doesn't
	 * matter until the on-page cell is set to WT_CELL_VALUE_OVFL_RM. That
	 * involves atomic operations which will act as our barrier. Regardless,
	 * we update the page footprint as part of this operation, which acts as
	 * a barrier as well.
	 *
	 * The update transaction ID choice is tricky, to work around an issue
	 * in variable-length column store. Imagine an overflow value with an
	 * RLE greater than 1. We append a copy to the end of an update chain,
	 * but it's possible it's the overflow value for more than one record,
	 * and appending it to the end of one record's update chain means a
	 * subsequent enter of a globally visible value to one of the records
	 * would allow the truncation of the overflow chain that leaves other
	 * records without a value. If appending such an overflow record, set
	 * the transaction ID to the first possible transaction ID. That ID is
	 * old enough to be globally visible, but we can use it as a flag if an
	 * update record cannot be discarded when truncating an update chain.
	 */
	WT_ERR(__wt_update_alloc(
	    session, tmp, &append, &size, WT_UPDATE_STANDARD));
	append->txnid = page->type == WT_PAGE_COL_VAR &&
	    __wt_cell_rle(unpack) > 1 ? WT_TXN_FIRST : WT_TXN_NONE;
	for (upd = upd_list; upd->next != NULL; upd = upd->next)
		;
	WT_PUBLISH(upd->next, append);

	__wt_cache_page_inmem_incr(session, page, size);

	*updp = append;

err:	__wt_scr_free(session, &tmp);
	return (ret);
}

/*
 * __ovfl_cache --
 *	Cache an overflow value.
 */
static int
__ovfl_cache(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_UPDATE *upd_list, WT_CELL_UNPACK *unpack)
{
	WT_OVFL_TRACK *track;
	WT_UPDATE *upd;

	/* Append a copy of the overflow value to the update list. */
	WT_RET(__ovfl_cache_append_update(
	    session, page, upd_list, unpack, &upd));

	/* Allocating tracking structures as necessary. */
	if (page->modify->ovfl_track == NULL)
		WT_RET(__wt_ovfl_track_init(session, page));
	track = page->modify->ovfl_track;

	/* Add the value's information to the update list. */
	WT_RET(__wt_realloc_def(session,
	    &track->remove_allocated, track->remove_next + 1, &track->remove));
	track->remove[track->remove_next].cell = unpack->cell;
	track->remove[track->remove_next].upd = upd;
	++track->remove_next;

	return (0);
}

/*
 * __wt_ovfl_remove --
 *	Remove an overflow value.
 */
int
__wt_ovfl_remove(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_UPDATE *upd_list, WT_CELL_UNPACK *unpack)
{
	bool visible;

	/*
	 * This function solves a problem in reconciliation. The scenario is:
	 *     - reconciling a leaf page that references an overflow item
	 *     - the item is updated and the update committed
	 *     - a checkpoint runs, freeing the backing overflow blocks
	 *     - a snapshot transaction wants the original version of the item
	 *
	 * In summary, we may need the original version of an overflow item for
	 * a snapshot transaction after the item was deleted from a page that's
	 * subsequently been checkpointed, where the checkpoint must know about
	 * the freed blocks.  We don't have any way to delay a free of the
	 * underlying blocks until a particular set of transactions exit (and
	 * this shouldn't be a common scenario), so cache the overflow value in
	 * memory.
	 *
	 * This gets hard because the snapshot transaction reader might:
	 *     - search the WT_UPDATE list and not find an useful entry
	 *     - read the overflow value's address from the on-page cell
	 *     - go to sleep
	 *     - checkpoint runs, caches the overflow value, frees the blocks
	 *     - another thread allocates and overwrites the blocks
	 *     - the reader wakes up and reads the wrong value
	 *
	 * Use a read/write lock and the on-page cell to fix the problem: hold
	 * a write lock when changing the cell type from WT_CELL_VALUE_OVFL to
	 * WT_CELL_VALUE_OVFL_RM and hold a read lock when reading an overflow
	 * item.
	 *
	 * The read/write lock is per btree, but it could be per page or even
	 * per overflow item.  We don't do any of that because overflow values
	 * are supposed to be rare and we shouldn't see contention for the lock.
	 *
	 * Check for a globally visible update.  If there is a globally visible
	 * update, we don't need to cache the item because it's not possible for
	 * a running thread to have moved past it.
	 */
	switch (page->type) {
	case WT_PAGE_COL_VAR:
		visible = __ovfl_cache_col_visible(session, upd_list, unpack);
		break;
	case WT_PAGE_ROW_LEAF:
		visible = __ovfl_cache_row_visible(session, upd_list);
		break;
	WT_ILLEGAL_VALUE(session);
	}

	/*
	 * If there's no globally visible update, there's a reader in the system
	 * that might try and read the old value, cache it.
	 */
	if (!visible)
		WT_RET(__ovfl_cache(session, page, upd_list, unpack));

	/*
	 * Queue the on-page cell to be set to WT_CELL_VALUE_OVFL_RM and the
	 * underlying overflow value's blocks to be freed when reconciliation
	 * completes.
	 */
	return (__wt_ovfl_discard_add(session, page, unpack->cell));
}

/*
 * __wt_ovfl_discard --
 *	Discard an on-page overflow value, and reset the page's cell.
 */
int
__wt_ovfl_discard(WT_SESSION_IMPL *session, WT_CELL *cell)
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_CELL_UNPACK *unpack, _unpack;

	btree = S2BT(session);
	bm = btree->bm;
	unpack = &_unpack;

	__wt_cell_unpack(cell, unpack);

	/*
	 * Finally remove overflow key/value objects, called when reconciliation
	 * finishes after successfully writing a page.
	 *
	 * Keys must have already been instantiated and value objects must have
	 * already been cached (if they might potentially still be read by any
	 * running transaction).
	 *
	 * Acquire the overflow lock to avoid racing with a thread reading the
	 * backing overflow blocks.
	 */
	__wt_writelock(session, &btree->ovfl_lock);

	switch (unpack->raw) {
	case WT_CELL_KEY_OVFL:
		__wt_cell_type_reset(session,
		    unpack->cell, WT_CELL_KEY_OVFL, WT_CELL_KEY_OVFL_RM);
		break;
	case WT_CELL_VALUE_OVFL:
		__wt_cell_type_reset(session,
		    unpack->cell, WT_CELL_VALUE_OVFL, WT_CELL_VALUE_OVFL_RM);
		break;
	WT_ILLEGAL_VALUE(session);
	}

	__wt_writeunlock(session, &btree->ovfl_lock);

	/* Free the backing disk blocks. */
	return (bm->free(bm, session, unpack->data, unpack->size));
}
