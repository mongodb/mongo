/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_ovfl_in --
 *	Read an overflow item from the disk.
 */
int
__wt_ovfl_in(
    WT_SESSION_IMPL *session, WT_ITEM *store, const uint8_t *addr, uint32_t len)
{
	WT_BTREE *btree;

	btree = session->btree;

	WT_BSTAT_INCR(session, overflow_read);

	/*
	 * Read an overflow page, using an address from a page for which we
	 * (better) have a hazard reference.
	 *
	 * Overflow reads are synchronous. That may bite me at some point, but
	 * WiredTiger supports large page sizes, overflow items should be rare.
	 */
	WT_RET(__wt_bm_read(session, store, addr, len));

	/* Reference the start of the data and set the data's length. */
	store->data = WT_PAGE_HEADER_BYTE(btree, store->mem);
	store->size = ((WT_PAGE_HEADER *)store->mem)->u.datalen;

	return (0);
}

/*
 * __wt_ovfl_in_cache --
 *	Read an overflow item from the disk and prepend it to a WT_UPDATE list.
 */
int
__wt_ovfl_in_cache(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_UPDATE *upd_arg, const uint8_t *addr, uint32_t len)
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
	 * a snapshot transaction, after it's been deleted from the checkpoint
	 * tree.  We don't have any way to delay a free of the underlying blocks
	 * until a particular set of transactions exit, and this isn't a common
	 * scenario: read the original overflow item and stash it at the end of
	 * the WT_UPDATE list with a transaction ID guaranteeing no reader will
	 * go past the entry.
	 *
	 * First, check to see if there's an update everybody can see (there may
	 * already be one, else the one this routine adds will be visible); this
	 * check prevents us from repeatedly reading/caching overflow items.
	 *
	 * If we don't find anything globally visible on the first pass, update
	 * our cached copy of the oldest ID required in the system and rescan.
	 * This is worthwhile for two reasons: (a) we may avoid the read, and
	 * (b) if there was a previous reconciliation of this page, the
	 * overflow item referenced on the page may already have been freed,
	 * and the copy we installed found to be obsolete by another thread
	 * that has a more recent copy of the oldest ID.
	 */
	for (pass = 0; pass < 2; pass++) {
		for (upd = upd_arg;; upd = upd->next) {
			if (__wt_txn_visible_all(session, upd->txnid))
				return (0);
			if (upd->next == NULL)
				break;
		}
		if (pass == 0)
			__wt_txn_get_oldest(session);
	}

	/* Read in the overflow item and copy it into a WT_UPDATE structure. */
	WT_ERR(__wt_ovfl_in(session, &tmp, addr, len));
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

	/* Ensure the write makes it out before our caller frees anything. */
	WT_WRITE_BARRIER();

	if (0)
err:		__wt_free(session, new);

	__wt_buf_free(session, &tmp);
	return (ret);
}
