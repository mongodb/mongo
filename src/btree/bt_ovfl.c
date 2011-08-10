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
__wt_ovfl_in(WT_SESSION_IMPL *session, WT_OFF *ovfl, WT_BUF *store)
{
	/*
	 * Read an overflow page, using an overflow structure from a page for
	 * which we (better) have a hazard reference.
	 *
	 * Overflow reads are synchronous. That may bite me at some point, but
	 * WiredTiger supports large page sizes, and overflow items should be
	 * rare.
	 */
	WT_BSTAT_INCR(session, overflow_read);

	/* Re-allocate buffer memory as necessary to hold the overflow page. */
	WT_RET(__wt_buf_initsize(session, store, ovfl->size));

	/* Read the page, decompressing if needed. */
	WT_RET(__wt_disk_read_scr(session, store, ovfl->addr, &ovfl->size));

	/* Reference the start of the data and set the data's length. */
	store->data = WT_PAGE_DISK_BYTE(store->mem);
	store->size = ((WT_PAGE_DISK *)store->mem)->u.datalen;

	return (0);
}
