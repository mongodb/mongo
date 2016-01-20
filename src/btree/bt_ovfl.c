/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
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

	WT_STAT_FAST_DATA_INCR(session, cache_read_overflow);

	return (0);
}

/*
 * __wt_ovfl_read --
 *	Bring an overflow item into memory.
 */
int
__wt_ovfl_read(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_CELL_UNPACK *unpack, WT_ITEM *store)
{
	WT_DECL_RET;

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
	 * and we will be passed a page so we can look-aside into the cache of
	 * such values.
	 *
	 * Acquire the overflow lock, and retest the on-page cell's value inside
	 * the lock.
	 */
	WT_RET(__wt_readlock(session, S2BT(session)->ovfl_lock));
	ret = __wt_cell_type_raw(unpack->cell) == WT_CELL_VALUE_OVFL_RM ?
	    __wt_ovfl_txnc_search(page, unpack->data, unpack->size, store) :
	    __ovfl_read(session, unpack->data, unpack->size, store);
	WT_TRET(__wt_readunlock(session, S2BT(session)->ovfl_lock));

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
	    upd != NULL &&		/* Sanity: upd should always be set. */
	    __wt_txn_visible_all(session, upd->txnid))
		return (true);
	return (false);
}

/*
 * __ovfl_cache_row_visible --
 *	row-store: check for a globally visible update.
 */
static bool
__ovfl_cache_row_visible(WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip)
{
	WT_UPDATE *upd;

	/* Check to see if there's a globally visible update. */
	for (upd = WT_ROW_UPDATE(page, rip); upd != NULL; upd = upd->next)
		if (__wt_txn_visible_all(session, upd->txnid))
			return (true);

	return (false);
}

/*
 * __ovfl_cache --
 *	Cache a deleted overflow value.
 */
static int
__ovfl_cache(WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL_UNPACK *unpack)
{
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	size_t addr_size;
	const uint8_t *addr;

	addr = unpack->data;
	addr_size = unpack->size;

	WT_RET(__wt_scr_alloc(session, 1024, &tmp));

	/* Enter the value into the overflow cache. */
	WT_ERR(__ovfl_read(session, addr, addr_size, tmp));
	WT_ERR(__wt_ovfl_txnc_add(
	    session, page, addr, addr_size, tmp->data, tmp->size));

err:	__wt_scr_free(session, &tmp);
	return (ret);
}

/*
 * __wt_ovfl_cache --
 *	Handle deletion of an overflow value.
 */
int
__wt_ovfl_cache(WT_SESSION_IMPL *session,
    WT_PAGE *page, void *cookie, WT_CELL_UNPACK *vpack)
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
		visible = __ovfl_cache_col_visible(session, cookie, vpack);
		break;
	case WT_PAGE_ROW_LEAF:
		visible = __ovfl_cache_row_visible(session, page, cookie);
		break;
	WT_ILLEGAL_VALUE(session);
	}

	/*
	 * If there's no globally visible update, there's a reader in the system
	 * that might try and read the old value, cache it.
	 */
	if (!visible) {
		WT_RET(__ovfl_cache(session, page, vpack));
		WT_STAT_FAST_DATA_INCR(session, cache_overflow_value);
	}

	/*
	 * Queue the on-page cell to be set to WT_CELL_VALUE_OVFL_RM and the
	 * underlying overflow value's blocks to be freed when reconciliation
	 * completes.
	 */
	return (__wt_ovfl_discard_add(session, page, vpack->cell));
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
	WT_DECL_RET;

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
	WT_RET(__wt_writelock(session, btree->ovfl_lock));

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

	WT_TRET(__wt_writeunlock(session, btree->ovfl_lock));

	/* Free the backing disk blocks. */
	WT_TRET(bm->free(bm, session, unpack->data, unpack->size));

	return (ret);
}
