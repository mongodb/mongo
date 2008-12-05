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
	u_int32_t checksum, i;
	u_int8_t *p;
	int ret;

	hdr = page;
	ret = 0;

	switch (hdr->type) {
	case WT_PAGE_BTREE_ROOT:
	case WT_PAGE_BTREE_INTERNAL:
	case WT_PAGE_BTREE_LEAF:
	case WT_PAGE_BTREE_OVERFLOW:
		break;
	default:
		__wt_db_errx(db,
		    "page at address %lu has an invalid type of %lu",
		    (u_long)addr, (u_long)hdr->type);
		ret = WT_ERROR;
	}

	if (hdr->flags != 0) {
		__wt_db_errx(db,
		    "page at address %lu has an invalid flag value of %lu",
		    (u_long)addr, (u_long)hdr->flags);
		ret = WT_ERROR;
	}

	if (hdr->entries == 0) {
		__wt_db_errx(db,
		    "page at addr %lu has no entries", (u_long)addr);
		ret = WT_ERROR;
	}

	switch (hdr->type) {
	case WT_PAGE_BTREE_LEAF:
		for (p = (u_int8_t *)hdr + WT_HDR_SIZE, i = 0;
		    i < hdr->entries;
		    p += WT_ITEM_SPACE_REQ(item->len), --i) {
			item = (WT_ITEM *)p;
			switch (item->type) {
			case WT_ITEM_STANDARD:
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

	return (ret);
}
