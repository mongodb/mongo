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
 * __wt_db_ovfl_copy --
 *	Copy an overflow item in the database, returning the starting
 *	addr.  This routine is used when an overflow item is promoted
 *	to an internal page.
 */
int
__wt_db_ovfl_copy(DB *db, WT_ITEM_OVFL *from, WT_ITEM_OVFL *copy)
{
	DBT dbt;
	WT_PAGE *ovfl_page;
	u_int32_t frags;
	int ret, tret;

	/* Read in the overflow record. */
	WT_OVERFLOW_BYTES_TO_FRAGS(db, from->len, frags);
	if ((ret = __wt_db_page_in(db, from->addr, frags, &ovfl_page, 0)) != 0)
		return (ret);

	/*
	 * Copy the overflow record to a new location, and set our return
	 * information.
	 */
	WT_CLEAR(dbt);
	dbt.data = WT_PAGE_BYTE(ovfl_page);
	dbt.size = from->len;
	ret = __wt_db_ovfl_write(db, &dbt, &copy->addr);
	copy->len = from->len;

	/* Discard the overflow record. */
	if ((tret = __wt_db_page_out(db, ovfl_page, 0)) != 0 && ret == 0)
		ret = tret;

	return (ret);
}

/*
 * __wt_db_ovfl_write --
 *	Store an overflow item in the database, returning the starting
 *	addr.
 */
int
__wt_db_ovfl_write(DB *db, DBT *dbt, u_int32_t *addrp)
{
	WT_PAGE *page;
	u_int32_t frags;
	int ret;

	/* Allocate a chunk of file space. */
	WT_OVERFLOW_BYTES_TO_FRAGS(db, dbt->size, frags);
	if ((ret = __wt_db_page_alloc(db, frags, &page)) != 0)
		return (ret);

	/* Initialize the page and copy the overflow item in. */
	page->hdr->type = WT_PAGE_OVFL;
	page->hdr->u.datalen = dbt->size;
	page->hdr->prntaddr =
	    page->hdr->prevaddr = page->hdr->nextaddr = WT_ADDR_INVALID;
	memcpy(WT_PAGE_BYTE(page), dbt->data, dbt->size);

	/* The caller wants the addr. */
	*addrp = page->addr;

	/* Write the overflow item back to the file. */
	return (__wt_db_page_out(db, page, WT_MODIFIED));
}
