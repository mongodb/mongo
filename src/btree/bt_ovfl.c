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
    WT_PAGE *page, WT_UPDATE *upd, const uint8_t *addr, uint32_t len)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_ITEM tmp;
	WT_UPDATE *new;

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
	 */
	for (;; upd = upd->next) {
		if (__wt_txn_visible_all(session, upd->txnid))
			return (0);
		if (upd->next == NULL)
			break;
	}

	WT_BSTAT_INCR(session, overflow_value_cache);

	/* Read in the overflow item and copy it into a WT_UPDATE structure. */
	WT_ERR(__wt_ovfl_in(session, &tmp, addr, len));
	WT_ERR(__wt_calloc(session, 1, sizeof(WT_UPDATE) + tmp.size, &new));
	new->size = tmp.size;
	new->txnid = WT_TXN_NONE;			/* Visible to all */
	memcpy(WT_UPDATE_DATA(new), tmp.data, tmp.size);

	/* Add the value as the oldest record in the WT_UPDATE list. */
	upd->next = new;

	/* Update the in-memory footprint. */
	__wt_cache_page_inmem_incr(session, page, sizeof(WT_UPDATE) + tmp.size);

	/* Ensure the write makes it out before our caller frees anything. */
	WT_WRITE_BARRIER();

	if (0)
err:		__wt_free(session, new);

	__wt_buf_free(session, &tmp);
	return (ret);
}
