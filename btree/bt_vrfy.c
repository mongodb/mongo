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
 * __wt_bt_page_verify --
 *	Verify a single Btree page.
 */
int
__wt_bt_page_verify(DB *db, u_int32_t addr, void *page)
{
	IENV *ienv;
	WT_ITEM *item;
	WT_PAGE_HDR *hdr;
	u_int32_t i;
	u_int8_t *p;
	int ret;

	hdr = page;
	ret = 0;

	switch (hdr->type) {
	case WT_PAGE_OVFL:
	case WT_PAGE_ROOT:
	case WT_PAGE_INT:
	case WT_PAGE_LEAF:
	case WT_PAGE_DUP_ROOT:
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
		break;
	default:
		__wt_db_errx(db,
		    "page at address %lu has an invalid type of %lu",
		    (u_long)addr, (u_long)hdr->type);
		ret = WT_ERROR;
	}

	if (hdr->type != WT_PAGE_OVFL && hdr->u.entries == 0) {
		__wt_db_errx(db,
		    "page at addr %lu has no entries", (u_long)addr);
		ret = WT_ERROR;
	}

	switch (hdr->type) {
	case WT_PAGE_LEAF:
		for (p = WT_PAGE_DATA(hdr), i = 0;
		    i < hdr->u.entries;
		    p += WT_ITEM_SPACE_REQ(item->len), --i) {
			item = (WT_ITEM *)p;
			switch (item->type) {
			case WT_ITEM_KEY:
			case WT_ITEM_DATA:
			case WT_ITEM_DUP:
			case WT_ITEM_KEY_OVFL:
			case WT_ITEM_DATA_OVFL:
			case WT_ITEM_DUP_OVFL:
				break;
			default:
				__wt_db_errx(db,
		    "item %lu on page at addr %lu has an illegal type of %lu", 
				    (u_long)i,
				    (u_long)addr, (u_long)item->type);
				ret = WT_ERROR;
			}
			if (item->unused[0] != 0 ||
			    item->unused[1] != 0 || item->unused[2] != 0) {
				__wt_db_errx(db,
	    "item %lu on page at addr %lu has a corrupted item structure", 
				    (u_long)i, (u_long)addr);
				ret = WT_ERROR;
			}
		}
	}

	if (hdr->unused[0] != 0 ||
	    hdr->unused[1] != 0 || hdr->unused[2] != 0) {
		__wt_db_errx(db,
		    "header unused fields not zero'd", (u_long)addr);
		ret = WT_ERROR;
	}

	return (ret);
}
