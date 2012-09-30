/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __ovfl_addr_cleared --
 *	Return if an overflow address has been cleared.
 */
static inline int
__ovfl_addr_cleared(const uint8_t *addr, uint32_t addr_size)
{
	do {
		if (addr[--addr_size] != '\0')
			return (0);
	} while (addr_size != 0);
	return (1);
}

/*
 * __ovfl_read --
 *	Read an overflow item.
 */
static inline int
__ovfl_read(WT_SESSION_IMPL *session,
    WT_ITEM *store, const uint8_t *addr, uint32_t addr_size)
{
	WT_BTREE *btree;

	btree = session->btree;

	/*
	 * Read the overflow item, then reference the start of the data and
	 * set the data's length.
	 */
	WT_RET(__wt_bm_read(session, store, addr, addr_size));
	store->data = WT_PAGE_HEADER_BYTE(btree, store->mem);
	store->size = ((WT_PAGE_HEADER *)store->mem)->u.datalen;
	return (0);
}

/*
 * __wt_ovfl_in --
 *	Read an overflow item from the disk.
 */
int
__wt_ovfl_in(WT_SESSION_IMPL *session,
    WT_ITEM *store, const uint8_t *addr, uint32_t addr_size)
{
	WT_BTREE *btree;
	WT_DECL_RET;

	btree = session->btree;
	WT_BSTAT_INCR(session, overflow_read);

	/*
	 * Read an overflow item.
	 *
	 * Overflow reads are synchronous. That may bite me at some point, but
	 * WiredTiger supports large page sizes, overflow items should be rare.
	 *
	 * The address might have been cleared (if we race with reconciliation),
	 * restart those operations.
	 */
	__wt_readlock(session, btree->ovfl_lock);
	ret = __ovfl_addr_cleared(addr, addr_size) ?
	    WT_RESTART : __ovfl_read(session, store, addr, addr_size);
	__wt_rwunlock(session, btree->ovfl_lock);
	return (ret);
}

/*
 * __wt_ovfl_in_cache --
 *	Handle deletion of an overflow value.
 */
int
__wt_ovfl_in_cache(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_UPDATE *upd_arg, const uint8_t *addr, uint32_t addr_size)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_ITEM tmp;
	WT_UPDATE *new, *upd;
	int pass;

	btree = session->btree;
	WT_CLEAR(tmp);
	new = NULL;

	/*
	 * This function solves a problem in reconciliation. The scenario is:
	 *     - a leaf page that references an overflow item
	 *     - the item is updated, creating a WT_UPDATE chain
	 *     - the update is committed
	 *     - a checkpoint runs, freeing the backing overflow blocks
	 *     - a snapshot transaction wants the original version of the item
	 *
	 * In summary, we may need the original version of an overflow item for
	 * a snapshot transaction after it's been deleted from the checkpoint
	 * tree.  We don't have any way to delay a free of the underlying blocks
	 * until a particular set of transactions exit, and this isn't a common
	 * scenario: read the original overflow item and stash it at the end of
	 * the WT_UPDATE list with a transaction ID guaranteeing no reader will
	 * go past the entry.
	 *
	 * This gets hard because the snapshot transaction might:
	 *     - read through the WT_UPDATE list not finding an entry
	 *     - get the overflow value's address from the on-page cell
	 *     - go to sleep
	 *     - a checkpoint runs, creating a WT_UPDATE entry, frees the blocks
	 *     - another thread allocates and overwrites the blocks
	 *     - the reader wakes up and reads the wrong value
	 *
	 * To fix that problem, we use a read/write lock and the on-page cell to
	 * synchronize: hold a write lock when creating the WT_UPDATE entry, and
	 * inside the same lock clear the overflow cell's address, readers hold
	 * a read lock and restart operations if they find an address consisting
	 * entirely of nul bytes.
	 *
	 * The read/write lock is per btree, but it could be per page or even
	 * per overflow item.  We don't do any of that because overflow values
	 * are supposed to be rare and we shouldn't see contention for the lock.
	 *
	 * Note, the address better be on the page, it's a bad idea to copy an
	 * address and then use it, things will go bad.
	 */
	WT_ASSERT(session, !__wt_off_page(page, addr));

	/*
	 * Pages are repeatedly reconciled and we don't want to lock out readers
	 * every time we reconcile an overflow item on a page.  First, check if
	 * we've already handled this address.  If it appears work is required,
	 * lock and check again.
	 */
	if (__ovfl_addr_cleared(addr, addr_size))
		return (0);
	__wt_writelock(session, btree->ovfl_lock);
	if (__ovfl_addr_cleared(addr, addr_size))
		goto nowork;

	/*
	 * Second, enter this address into the tracking system.  We don't care
	 * about the data for the purposes of tracking the object, we're going
	 * to deal with readers needing a cached value using the WT_UPDATE
	 * chain rather than the reconciliation tracking system.  (We could do
	 * this in the reconciliation tracking system, but (1) it doesn't make
	 * the locking issues any simpler, (2) the cached value would persist,
	 * whereas cached values in the WT_UPDATE chain are discarded once there
	 * are no longer readers that need them, and (3) all readers must start
	 * checking the page's modification tracking system in this case, while
	 * readers already know all about WT_UPDATE chains.  So, the WT_UPDATE
	 * chain it is.)
	 */
	WT_ERR(__wt_rec_track_onpage_addr(session, page, addr, addr_size));

	/*
	 * Third, check to see if there's already an update everybody can see.
	 *
	 * If there's no globally visible update using our cached copy of the
	 * oldest ID required in the system, refresh that ID and rescan, it's
	 * cheaper than doing I/O.
	 */
	for (pass = 0; pass < 2; pass++) {
		for (upd = upd_arg;; upd = upd->next) {
			if (__wt_txn_visible_all(session, upd->txnid))
				goto nowork;
			if (upd->next == NULL)
				break;
		}
		if (pass == 0)
			__wt_txn_get_oldest(session);
	}

	/*
	 * OK, there's no globally visible update which means there's a reader
	 * in the system that just might try and read the old value.
	 *
	 * Read in the overflow item and copy it into a WT_UPDATE structure.
	 */
	WT_ERR(__ovfl_read(session, &tmp, addr, addr_size));
	WT_ERR(__wt_calloc(session, 1, sizeof(WT_UPDATE) + tmp.size, &new));
	new->size = tmp.size;
	new->txnid = WT_TXN_NONE;			/* Visible to all */
	memcpy(WT_UPDATE_DATA(new), tmp.data, tmp.size);

	/*
	 * XXX how do we prevent this sequence happening while we were reading
	 * the overflow item?
	 *
	 *  (a) the session with the oldest snap_min has moved on;
	 *  (b) another thread installed a new update and decided the
	 *      WT_UPDATE we are pointing to in "upd" is obsolete;
	 *  (c) we try to append our new item to a freed update node.
	 *
	 * This doesn't seem to be firing in practice (it hasn't been found in
	 * test/format), but it seems like a possibility.
	 *
	 * One idea would be to re-scan the list here, using the same logic
	 * as __wt_update_obsolete, and atomically swap in our new node.
	 * Even that could race with a truncating thread, though, so we would
	 * need to retry.
	 *
	 * (!) In fact, just with two threads racing to update the same item,
	 * they could call __wt_update_obsolete with different ideas about the
	 * oldest snap_min and choose different nodes as truncation point,
	 * leading to one freeing memory the other is reading.
	 */
#if 0
	do {
		for (upd = upd_arg;; upd = upd->next) {
			if (__wt_txn_visible_all(session, upd->txnid))
				goto err;
			if (upd->next == NULL)
				break;
		}
		if (WT_ATOMIC_CAS(upd->next, NULL, new))
			break;
		__wt_txn_get_oldest(session);
	} while (1);
#endif

	/* Add the value as the oldest record in the WT_UPDATE list. */
	upd->next = new;

	/* Update the in-memory footprint. */
	__wt_cache_page_inmem_incr(session, page, sizeof(WT_UPDATE) + tmp.size);
	WT_BSTAT_INCR(session, overflow_value_cache);

	/* Clear the on-page cell. */
	memset((uint8_t *)addr, 0, addr_size);

nowork:
	if (0)
err:		__wt_free(session, new);

	__wt_buf_free(session, &tmp);

	__wt_rwunlock(session, btree->ovfl_lock);
	return (ret);
}
