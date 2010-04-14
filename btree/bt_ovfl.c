/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
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
__wt_bt_ovfl_in(WT_TOC *toc, u_int32_t addr, u_int32_t len, WT_PAGE **pagep)
{
	DB *db;
	WT_PAGE *page;

	db = toc->db;

	WT_RET(__wt_page_in(toc, addr, WT_OVFL_BYTES(db, len), &page));

	/* Verify the page. */
	WT_ASSERT(toc->env, __wt_bt_verify_page(toc, page, NULL) == 0);

	*pagep = page;
	return (0);
}

/*
 * __wt_bt_ovfl_write --
 *	Store an overflow item in the database, returning the starting
 *	addr.
 */
int
__wt_bt_ovfl_write(WT_TOC *toc, DBT *dbt, u_int32_t *addrp)
{
	DB *db;
	WT_PAGE *page;

	db = toc->db;

	/* Allocate a chunk of file space. */
	WT_RET(__wt_page_alloc(toc, WT_OVFL_BYTES(db, dbt->size), &page));

	/* Initialize the page and copy the overflow item in. */
	page->hdr->type = WT_PAGE_OVFL;
	page->hdr->u.datalen = dbt->size;
	page->hdr->prntaddr =
	    page->hdr->prevaddr = page->hdr->nextaddr = WT_ADDR_INVALID;

	/* Return the page address to the caller. */
	*addrp = page->addr;

	/* Copy the record into place. */
	memcpy(WT_PAGE_BYTE(page), dbt->data, dbt->size);

	/* Write the overflow item back to the file. */
	return (__wt_bt_page_out(toc, page, WT_MODIFIED));
}

/*
 * __wt_bt_ovfl_copy --
 *	Copy an overflow item in the database, returning the starting
 *	addr.  This routine is used when an overflow item is promoted
 *	to an internal page.
 */
int
__wt_bt_ovfl_copy(WT_TOC *toc, WT_OVFL *from, WT_OVFL *copy)
{
	DBT dbt;
	WT_PAGE *ovfl_page;
	int ret;

	/* Read in the overflow record. */
	WT_RET(__wt_bt_ovfl_in(toc, from->addr, from->len, &ovfl_page));

	/*
	 * Copy the overflow record to a new location, and set our return
	 * information.
	 */
	WT_CLEAR(dbt);
	dbt.data = WT_PAGE_BYTE(ovfl_page);
	dbt.size = from->len;
	ret = __wt_bt_ovfl_write(toc, &dbt, &copy->addr);
	copy->len = from->len;

	/* Discard the overflow record. */
	WT_TRET(__wt_bt_page_out(toc, ovfl_page, 0));

	return (ret);
}

/*
 * __wt_bt_ovfl_to_dbt --
 *	Copy an overflow item into allocated memory in a DBT.
 */
int
__wt_bt_ovfl_to_dbt(WT_TOC *toc, WT_OVFL *ovfl, DBT *copy)
{
	DB *db;
	WT_PAGE *ovfl_page;
	int ret;

	db = toc->db;

	WT_RET(__wt_bt_ovfl_in(toc, ovfl->addr, ovfl->len, &ovfl_page));

	ret = __wt_bt_data_copy_to_dbt(
	    db, WT_PAGE_BYTE(ovfl_page), ovfl->len, copy);

	WT_TRET(__wt_bt_page_out(toc, ovfl_page, 0));

	return (ret);
}
