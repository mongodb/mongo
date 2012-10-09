/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
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

	btree = session->btree;

	/*
	 * Read the overflow item from the block manager, then reference the
	 * start of the data and set the data's length.
	 *
	 * Overflow reads are synchronous. That may bite me at some point, but
	 * WiredTiger supports large page sizes, overflow items should be rare.
	 */
	WT_RET(__wt_bm_read(session, store, addr, addr_size));
	store->data = WT_PAGE_HEADER_BYTE(btree, store->mem);
	store->size = ((WT_PAGE_HEADER *)store->mem)->u.datalen;
	return (0);
}

/*
 * __wt_ovfl_read --
 *	Bring an overflow item into memory.
 */
int
__wt_ovfl_read(WT_SESSION_IMPL *session, WT_ITEM *store, WT_CELL_UNPACK *unpack)
{
	WT_BTREE *btree;
	WT_DECL_RET;

	btree = session->btree;
	WT_BSTAT_INCR(session, overflow_read);

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
	__wt_readlock(session, btree->val_ovfl_lock);
	if (__wt_cell_type_raw(unpack->cell) == WT_CELL_VALUE_OVFL_RM)
		ret = WT_RESTART;
	else
		ret = __ovfl_read(session, store, unpack->data, unpack->size);
	__wt_rwunlock(session, btree->val_ovfl_lock);
	return (ret);
}

/*
 * __ovfl_cache_visible --
 *	Check to see if there's already an update everybody can see.
 */
static int
__ovfl_cache_visible(WT_SESSION_IMPL *session, WT_UPDATE *upd_arg)
{
	WT_UPDATE *upd;

	WT_ASSERT(session, upd_arg != NULL);

	/*
	 * Check to see if there's a globally visible update.  If there's no
	 * globally visible update using our cached copy of the oldest ID
	 * required in the system, refresh that ID and rescan, it's better
	 * than doing I/O and caching copies of an overflow record.
	 */
	for (upd = upd_arg; upd != NULL; upd = upd->next)
		if (__wt_txn_visible_all(session, upd->txnid))
			return (1);
	__wt_txn_get_oldest(session);
	for (upd = upd_arg; upd != NULL; upd = upd->next)
		if (__wt_txn_visible_all(session, upd->txnid))
			return (1);
	return (0);
}

/*
 * __val_ovfl_cache_col --
 *	Cache a deleted overflow value for a variable-length column-store.
 */
static int
__val_ovfl_cache_col(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_UPDATE *upd, WT_CELL_UNPACK *unpack)
{
	WT_DECL_RET;
	WT_ITEM value;
	const uint8_t *addr;
	uint32_t addr_size;

	addr = unpack->data;
	addr_size = unpack->size;

	WT_CLEAR(value);

	/*
	 * Column-store is harder than row_store: we're here because there's a
	 * reader in the system that might read the original version of an
	 * overflow record, which might match a number of records (for example,
	 * the original overflow value was for records 100-200, we've replaced
	 * each of those records individually, but there exists a reader that
	 * might read any one of those records, and all of those records have
	 * different WT_UPDATE entries with different transaction IDs.  Since
	 * it's infeasible to determine if there's a globally visible update
	 * for each reader for each record, we test one simple case, otherwise,
	 * we cache the record.
	 *
	 * Enter this address into the tracking system, discarding the blocks
	 * when reconciliation completes.  Unlike row-store, we include the
	 * underlying overflow value with the address because cached overflow
	 * values are recovered from the reconciliation tracking information,
	 * not from the normal insert/update structures we use in normal value
	 * changes.
	 */
#ifdef HAVE_DIAGNOSTIC
	{
	int found;
	WT_RET(__wt_rec_track_onpage_srch(
	    session, page, addr, addr_size, &found, NULL));
	WT_ASSERT(session, found == 0);
	}
#endif
#if 0
	/*
	 * Here's a quick test for a probably common case: a single matching
	 * record with a single, globally visible update.
	 */
	if (__wt_cell_rle(unpack) != 1 ||
	    upd == NULL ||				/* Sanity check. */
	    !__wt_txn_visible_all(session, upd->txnid))
		WT_ERR(__ovfl_read(session, &value, addr, addr_size));
#else
	WT_UNUSED(upd);
	WT_ERR(__ovfl_read(session, &value, addr, addr_size));
#endif

	WT_ERR(__wt_rec_track(session,
	    page, addr, addr_size, value.data, value.size, WT_TRK_ONPAGE));

	/* Update the in-memory footprint. */
	__wt_cache_page_inmem_incr(session, page, value.size);

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
	 * value in the page's reconciliation tracking information.  We handle
	 * the case where the record isn't found, but that should never happen,
	 * it indicates a fatal problem if it does.
	 */
	if (__wt_cell_type_raw(unpack->cell) != WT_CELL_VALUE_OVFL_RM)
		return (WT_RESTART);

	WT_RET(__wt_rec_track_onpage_srch(
	    session, page, unpack->data, unpack->size, &found, store));
	return (found ? 0 : WT_NOTFOUND);
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

	addr = unpack->data;
	addr_size = unpack->size;

	new = NULL;
	WT_CLEAR(value);

	/*
	 * Enter this address into the tracking system, discarding the blocks
	 * when reconciliation completes.
	 *
	 * We don't track the data (there's no reason to believe we'll re-use
	 * this overflow value), and we're going to deal with readers needing
	 * a cached value using the WT_UPDATE chain, not the reconciliation
	 * tracking system.  (Variable-length column-store does cache overflow
	 * values in the reconciliation tracking system, but (1) it doesn't make
	 * the locking issues any simpler, (2) the value persists until the page
	 * is evicted, whereas values in the WT_UPDATE chain are discarded once
	 * there are no longer readers needing them, and (3) readers have to
	 * explicitly check the page's reconciliation tracking system for the
	 * value, whereas readers already know all about WT_UPDATE chains.  So,
	 * the WT_UPDATE chain it is.)
	 */
#ifdef HAVE_DIAGNOSTIC
	{
	int found;
	WT_RET(__wt_rec_track_onpage_srch(
	    session, page, addr, addr_size, &found, NULL));
	WT_ASSERT(session, found == 0);
	}
#endif
	WT_ERR(__wt_rec_track(
	    session, page, addr, addr_size, NULL, 0, WT_TRK_ONPAGE));

	/*
	 * Check for a globally visible update; if there's no globally visible
	 * update, there's a reader in the system that might try and read the
	 * old value.
	 */
	if (__ovfl_cache_visible(session, WT_ROW_UPDATE(page, rip)))
		return (0);

	/*
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

	btree = session->btree;

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
	 * underlying blocks until a particular set of transactions exit, and
	 * this isn't a common scenario: instead, we cache the overflow item
	 * in memory.
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
	 * lock and check the on-page cell.  (Locking is required, it's possible
	 * we have cached information about what's in the on-page cell and it
	 * has changed.  Vanishingly unlikely, but I think it's possible.)
	 */
	if (unpack->raw == WT_CELL_VALUE_OVFL_RM)
		return (0);
	__wt_writelock(session, btree->val_ovfl_lock);
	if (__wt_cell_type_raw(unpack->cell) == WT_CELL_VALUE_OVFL_RM)
		goto err;

	/* Discard the original blocks and cache the deleted overflow value. */
	switch (page->type) {
	case WT_PAGE_COL_VAR:
		WT_ERR(__val_ovfl_cache_col(session, page, cookie, unpack));
		break;
	case WT_PAGE_ROW_LEAF:
		WT_ERR(__val_ovfl_cache_row(session, page, cookie , unpack));
		break;
	WT_ILLEGAL_VALUE_ERR(session);
	}
	WT_BSTAT_INCR(session, overflow_value_cache);

	/* Reset the on-page cell. */
	__wt_cell_type_reset(unpack->cell, WT_CELL_VALUE_OVFL_RM);

err:	__wt_rwunlock(session, btree->val_ovfl_lock);
	return (ret);
}
