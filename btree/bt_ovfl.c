/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_bt_ovfl_in --
 *	Read an overflow item from the cache.
 */
int
__wt_bt_ovfl_in(WT_TOC *toc, WT_OVFL *ovfl, WT_PAGE **pagep)
{
	DB *db;
	WT_PAGE *page;

	db = toc->db;

	/*
	 * Getting an overflow page from the cache, using an overflow structure
	 * on a page for which we have hazard reference.   If the page were to
	 * be rewritten/discarded from the cache while we're getting it, we can
	 * re-try -- re-trying is safe because our overflow information is from
	 * a page which can't be discarded because of our hazard reference.  If
	 * the page was re-written, our on-page overflow information will have
	 * been updated to the overflow page's new address.
	 */
	WT_RET_RESTART(__wt_page_in(
	    toc, ovfl->addr, WT_HDR_BYTES_TO_ALLOC(db, ovfl->size), &page, 0));

	*pagep = page;
	return (0);
}
