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

/*
 * __wt_bt_ovfl_write --
 *	Store overflow items in the database, returning the page addr.
 */
int
__wt_bt_ovfl_write(WT_TOC *toc, DBT *dbt, uint32_t *addrp)
{
	DB *db;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;

	db = toc->db;

	/* Allocate a chunk of file space. */
	WT_RET(__wt_page_alloc(
	    toc, WT_HDR_BYTES_TO_ALLOC(db, dbt->size), &page));

	/* Initialize the page and copy the overflow item in. */
	hdr = page->hdr;
	hdr->type = WT_PAGE_OVFL;
	hdr->level = WT_LLEAF;
	hdr->u.datalen = dbt->size;

	/* Return the page address to the caller. */
	*addrp = page->addr;

	/* Copy the record into place. */
	memcpy(WT_PAGE_BYTE(page), dbt->data, dbt->size);

	__wt_bt_page_out(toc, &page, WT_MODIFIED);
	return (0);
}

/*
 * __wt_bt_ovfl_copy --
 *	Copy an overflow item in the database, returning the starting
 *	addr; used when an overflow item is promoted to an internal page.
 */
int
__wt_bt_ovfl_copy(WT_TOC *toc, WT_OVFL *from, WT_OVFL *copy)
{
	DBT dbt;
	WT_PAGE *ovfl_page;
	int ret;

	/* Read in the overflow record. */
	WT_RET(__wt_bt_ovfl_in(toc, from, &ovfl_page));

	/*
	 * Copy the overflow record to a new location, and set our return
	 * information.
	 */
	WT_CLEAR(dbt);
	dbt.data = WT_PAGE_BYTE(ovfl_page);
	dbt.size = from->size;
	ret = __wt_bt_ovfl_write(toc, &dbt, &copy->addr);
	copy->size = from->size;

	__wt_bt_page_out(toc, &ovfl_page, 0);
	return (ret);
}
