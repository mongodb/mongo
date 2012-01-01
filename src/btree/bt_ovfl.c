/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_ovfl_in --
 *	Read an overflow item from the disk.
 */
int
__wt_ovfl_in(
    WT_SESSION_IMPL *session, WT_BUF *store, const uint8_t *addr, uint32_t len)
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
