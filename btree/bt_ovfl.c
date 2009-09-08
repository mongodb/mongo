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
__wt_bt_ovfl_in(DB *db, u_int32_t addr, u_int32_t len, WT_PAGE **pagep)
{
	ENV *env;
	WT_PAGE *page;
	WT_STOC *stoc;

	env = db->env;
	stoc = db->idb->stoc;

	WT_RET(__wt_cache_in(
	    stoc, WT_ADDR_TO_OFF(db, addr), WT_OVFL_BYTES(db, len), 0, &page));

	/* Verify the page. */
	WT_ASSERT(env, __wt_bt_verify_page(db, page, NULL, NULL) == 0);

	*pagep = page;
	return (0);
}

/*
 * __wt_bt_ovfl_write --
 *	Store an overflow item in the database, returning the starting
 *	addr.
 */
int
__wt_bt_ovfl_write(DB *db, DBT *dbt, u_int32_t *addrp)
{
	WT_PAGE *page;
	WT_STOC *stoc;

	stoc = db->idb->stoc;

	/* Allocate a chunk of file space. */
	WT_RET(__wt_cache_alloc(stoc, WT_OVFL_BYTES(db, dbt->size), &page));

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
	return (__wt_bt_page_out(db, page, WT_MODIFIED));
}

/*
 * __wt_bt_ovfl_copy --
 *	Copy an overflow item in the database, returning the starting
 *	addr.  This routine is used when an overflow item is promoted
 *	to an internal page.
 */
int
__wt_bt_ovfl_copy(DB *db, WT_ITEM_OVFL *from, WT_ITEM_OVFL *copy)
{
	DBT dbt;
	WT_PAGE *ovfl_page;
	int ret;

	/* Read in the overflow record. */
	WT_RET(__wt_bt_ovfl_in(db, from->addr, from->len, &ovfl_page));

	/*
	 * Copy the overflow record to a new location, and set our return
	 * information.
	 */
	WT_CLEAR(dbt);
	dbt.data = WT_PAGE_BYTE(ovfl_page);
	dbt.size = from->len;
	ret = __wt_bt_ovfl_write(db, &dbt, &copy->addr);
	copy->len = from->len;

	/* Discard the overflow record. */
	WT_TRET(__wt_bt_page_out(db, ovfl_page, 0));

	return (ret);
}

/*
 * __wt_bt_ovfl_to_dbt --
 *	Copy an overflow item into allocated memory in a DBT.
 */
int
__wt_bt_ovfl_to_dbt(DB *db, WT_ITEM_OVFL *ovfl, DBT *copy)
{
	WT_PAGE *ovfl_page;
	int ret;

	WT_RET(__wt_bt_ovfl_in(db, ovfl->addr, ovfl->len, &ovfl_page));

	ret = __wt_bt_data_copy_to_dbt(
	    db, WT_PAGE_BYTE(ovfl_page), ovfl->len, copy);

	WT_TRET(__wt_bt_page_out(db, ovfl_page, 0));

	return (ret);
}

/*
 * __wt_bt_ovfl_to_indx --
 *	Copy an overflow item into allocated memory in a WT_INDX
 */
int
__wt_bt_ovfl_to_indx(DB *db, WT_PAGE *page, WT_INDX *ip)
{
	ENV *env;
	WT_PAGE *ovfl_page;

	env = db->env;

	WT_RET(
	    __wt_bt_ovfl_in(db, WT_INDX_OVFL_ADDR(ip), ip->size, &ovfl_page));

	WT_RET(__wt_calloc(env, ip->size, 1, &ip->data));
	memcpy(ip->data, WT_PAGE_BYTE(ovfl_page), ip->size);

	WT_RET(__wt_bt_page_out(db, ovfl_page, 0));

	F_SET(ip, WT_ALLOCATED);
	F_SET(page, WT_ALLOCATED);

	return (0);
}
