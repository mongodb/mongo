/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
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
    WT_ITEM *store, const uint8_t *addr, uint32_t addr_size)
{
	WT_BTREE *btree;

	btree = S2BT(session);

	/*
	 * Read the overflow item from the block manager, then reference the
	 * start of the data and set the data's length.
	 *
	 * Overflow reads are synchronous. That may bite me at some point, but
	 * WiredTiger supports large page sizes, overflow items should be rare.
	 */
	WT_RET(__wt_bt_read(session, store, addr, addr_size));
	store->data = WT_PAGE_HEADER_BYTE(btree, store->mem);
	store->size = ((WT_PAGE_HEADER *)store->mem)->u.datalen;
	return (0);
}

/*
 * __wt_ovfl_read --
 *	Bring an overflow item into memory.
 */
int
__wt_ovfl_read(WT_SESSION_IMPL *session, WT_CELL_UNPACK *unpack, WT_ITEM *store)
{
	WT_BTREE *btree;
	WT_DECL_RET;

	btree = S2BT(session);
	WT_DSTAT_INCR(session, cache_read_overflow);

	/*
	 * The cell type might have been reset (if we race with reconciliation),
	 * restart those operations.  It's tempting to pass the page down into
	 * this function and look-aside into the area where values are cached
	 * instead of returning restart, but that won't work for row-store: the
	 * code caching updated overflow values doesn't bother if it finds a
	 * globally visible update in the update chain, and our search for a
	 * row's value might have not have seen that update, so we must restart
	 * the search from the beginning.
	 */
	WT_RET(__wt_readlock(session, btree->val_ovfl_lock));
	if (__wt_cell_type_raw(unpack->cell) == WT_CELL_VALUE_OVFL_RM)
		ret = WT_RESTART;
	else
		ret = __ovfl_read(session, store, unpack->data, unpack->size);
	WT_TRET(__wt_rwunlock(session, btree->val_ovfl_lock));
	return (ret);
}

/*
 * __ovfl_cache_col_visible --
 *	Check to see if there's already an update everybody can see.
 */
static int
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
	 * different WT_UPDATE entries with different transaction IDs.  Since
	 * it's infeasible to determine if there's a globally visible update
	 * for each reader for each record, we test the simple case where a
	 * single record has a single, globally visible update.  If that fails,
	 * cache the value.
	 */
	if (__wt_cell_rle(unpack) == 1 &&
	    upd != NULL &&		/* Sanity: upd should always be set. */
	    __wt_txn_visible_all(session, upd->txnid))
		return (1);
	return (0);
}

/*
 * __val_ovfl_cache_col --
 *	Cache a deleted overflow value for a variable-length column-store.
 */
static int
__val_ovfl_cache_col(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL_UNPACK *unpack)
{
	WT_DECL_RET;
	WT_ITEM value;
	const uint8_t *addr;
	uint32_t addr_size;

	WT_CLEAR(value);
	addr = unpack->data;
	addr_size = unpack->size;

	/*
	 * Because column-store values potentially match some number of records,
	 * there's no single WT_UPDATE chain we can use to cache the value, so
	 * we enter the value into the reconciliation tracking system.
	 */
	WT_ERR(__ovfl_read(session, &value, addr, addr_size));
	WT_ERR(__wt_rec_track(session, page, addr, addr_size,
	    value.data, value.size, WT_TRK_ONPAGE | WT_TRK_OVFL_VALUE));

err:	__wt_buf_free(session, &value);
	return (ret);
}

/*
 * __wt_ovfl_cache_col_restart --
 *	Handle restart of a variable-length column-store overflow read.
 */
int
__wt_ovfl_cache_col_restart(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_CELL_UNPACK *unpack, WT_ITEM *store)
{
	int found;

	/*
	 * A variable-length column-store overflow read returned restart: check
	 * the on-page cell (for sanity, this is currently the only reason an
	 * overflow read might return restart), then look up the cached overflow
	 * value in the page's reconciliation tracking information.
	 */
	if (__wt_cell_type_raw(unpack->cell) != WT_CELL_VALUE_OVFL_RM)
		return (WT_RESTART);

	found =
	    __wt_rec_track_ovfl_srch(page, unpack->data, unpack->size, store);
	WT_ASSERT(session, found == 1);
	WT_ASSERT(session, store->size != 0);

	/*
	 * We handle the case where the record isn't found, but that should
	 * never happen, it indicates a fatal problem if it does.
	 */
	return (found ? 0 : WT_NOTFOUND);
}

/*
 * __ovfl_cache_row_visible --
 *	Check to see if there's already an update everybody can see.
 */
static int
__ovfl_cache_row_visible(WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip)
{
	WT_UPDATE *first, *upd;

	first = WT_ROW_UPDATE(page, rip);
	WT_ASSERT(session, first != NULL);

	/* Check to see if there's a globally visible update. */
	for (upd = first; upd != NULL; upd = upd->next)
		if (__wt_txn_visible_all(session, upd->txnid))
			return (1);

	return (0);
}

/*
 * __val_ovfl_cache_row --
 *	Cache a deleted overflow value for a row-store.
 */
static int
__val_ovfl_cache_row(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_ROW *rip, WT_CELL_UNPACK *unpack)
{
	WT_DECL_RET;
	WT_ITEM value;
	WT_UPDATE *upd, *new;
	size_t upd_size;
	uint32_t addr_size;
	const uint8_t *addr;

	WT_CLEAR(value);
	new = NULL;
	addr = unpack->data;
	addr_size = unpack->size;

	/*
	 * We handle readers needing cached values using the WT_UPDATE chain,
	 * which works because there's a single WT_UPDATE chain per key.  We
	 * can't do that for variable-length column-stores, and so it caches
	 * values in the reconciliation tracking system.  We do it differently
	 * here because using the reconciliation tracking system (1) doesn't
	 * make the locking issues any simpler, (2) means the value persists
	 * until the page is evicted, whereas values in the WT_UPDATE chain are
	 * discarded once readers no longer need them, and (3) readers have to
	 * search the page's reconciliation tracking system for the value,
	 * whereas readers already know how to search WT_UPDATE chains.  So,
	 * the WT_UPDATE chain it is.)
	 *
	 * Read in the overflow item and copy it into a WT_UPDATE structure.
	 * We make the entry visible to all, guaranteeing that no reader will
	 * ever get past this entry, to the page.
	 */
	WT_ERR(__ovfl_read(session, &value, addr, addr_size));
	WT_ERR(__wt_update_alloc(session, &value, &new, &upd_size));
	new->txnid = WT_TXN_NONE;

	/*
	 * Append the WT_UPDATE structure to the end of the list, other entries
	 * should override it.  We are appending to the list, which matters: if
	 * we weren't appending to the list we'd potentially collide with other
	 * threads updating the list (we don't set the write generation number,
	 * which means other threads won't notice if we changed the update list
	 * underneath  them).  We still have to serialize the operation because
	 * another thread might truncate the list while we're walking it; walk
	 * from the beginning after we acquire the lock so we can't overlap with
	 * a truncation.
	 */
	__wt_spin_lock(session, &S2C(session)->serial_lock);
	for (upd = WT_ROW_UPDATE(page, rip);; upd = upd->next)
		if (upd->next == NULL) {
			upd->next = new;
			break;
		}
	__wt_spin_unlock(session, &S2C(session)->serial_lock);

	/* Update the in-memory footprint. */
	__wt_cache_page_inmem_incr(session, page, upd_size);

	if (0)
err:		__wt_free(session, new);

	__wt_buf_free(session, &value);
	return (ret);
}

/*
 * __wt_ovfl_cache --
 *	Handle deletion of an overflow value.
 */
int
__wt_val_ovfl_cache(WT_SESSION_IMPL *session,
    WT_PAGE *page, void *cookie, WT_CELL_UNPACK *unpack)
{
	WT_BTREE *btree;
	WT_DECL_RET;

	btree = S2BT(session);

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
	 * this isn't a common scenario), so cache the overflow value in memory.
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
	 * a write lock when creating the cached copy and resetting the on-page
	 * cell type from WT_CELL_VALUE_OVFL to WT_CELL_VALUE_OVFL_RM, hold a
	 * read lock when reading an overflow item, and return WT_RESTART if
	 * about to read an overflow item based on a WT_CELL_VALUE_OVFL_RM cell.
	 *
	 * The read/write lock is per btree, but it could be per page or even
	 * per overflow item.  We don't do any of that because overflow values
	 * are supposed to be rare and we shouldn't see contention for the lock.
	 *
	 * Pages are repeatedly reconciled and we don't want to lock out readers
	 * every time we reconcile an overflow item on a page.  Check if we've
	 * already cached this overflow value, and if work appears required,
	 * lock and check again.  (Locking is required, it's possible we have
	 * cached information about what's in the on-page cell and it's changed.
	 * Vanishingly unlikely, but I think it's possible.)
	 */
	if (unpack->raw == WT_CELL_VALUE_OVFL_RM)
		return (0);
	WT_RET(__wt_writelock(session, btree->val_ovfl_lock));
	if (__wt_cell_type_raw(unpack->cell) == WT_CELL_VALUE_OVFL_RM)
		goto err;

	/*
	 * Check for a globally visible update; if there's no globally visible
	 * update, there's a reader in the system that might try and read the
	 * old value, cache the deleted overflow value.
	 */
	switch (page->type) {
	case WT_PAGE_COL_VAR:
		if (__ovfl_cache_col_visible(session, cookie, unpack))
			break;
		WT_ERR(__val_ovfl_cache_col(session, page, unpack));
		WT_DSTAT_INCR(session, cache_overflow_value);
		break;
	case WT_PAGE_ROW_LEAF:
		if (__ovfl_cache_row_visible(session, page, cookie))
			break;
		WT_ERR(__val_ovfl_cache_row(session, page, cookie, unpack));
		WT_DSTAT_INCR(session, cache_overflow_value);
		break;
	WT_ILLEGAL_VALUE_ERR(session);
	}

	/*
	 * Reset the on-page cell, even if we didn't cache the overflow value.
	 * There may be a sleeping thread of control that's already walked the
	 * WT_UPDATE list and will read the on-page cell when it wakes up, and
	 * the cell has to be reset in order to cause that thread to return
	 * WT_RESTART to its caller.
	 */
	__wt_cell_type_reset(unpack->cell, WT_CELL_VALUE_OVFL_RM);

err:	WT_TRET(__wt_rwunlock(session, btree->val_ovfl_lock));
	return (ret);
}
