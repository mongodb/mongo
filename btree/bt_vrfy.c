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
 * __wt_db_page_verify --
 *	Verify a single Btree page.
 */
int
__wt_db_page_verify(DB *db, u_int32_t addr, WT_PAGE_HDR *hdr)
{
	IENV *ienv;
	WT_ITEM *item;
	u_int32_t i;
	u_int8_t *p;
	int ret;

	ret = 0;

	/*
	 * FUTURE:
	 * Check the LSN against the existing log files.
	 */

	switch (hdr->type) {
	case WT_PAGE_OVFL:
		if (hdr->u.entries == 0) {
			__wt_db_errx(db,
			    "page at addr %lu has no entries", (u_long)addr);
			ret = WT_ERROR;
		}
		/* FALLTHROUGH */
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
		return (WT_ERROR);
	}

	if (hdr->unused[0] != '\0' ||
	    hdr->unused[1] != '\0' || hdr->unused[2] != '\0') {
		__wt_db_errx(db,
		    "page at address %lu has non-zero unused header fields");
		return (WT_ERROR);
	}

	/*
	 * We've already verified the checksum, that gets done when we read
	 * the page.
	 */

	/*
	 * Most pages are sets of items.  Walk the items on the page and
	 * make sure we can read them without danger.
	 */
	if (hdr->type != WT_PAGE_OVFL && (ret = __wt_db_item_walk(hdr)) != 0)
		return (ret);
			
	switch (hdr->type) {
	case WT_PAGE_LEAF:
		for (p = WT_PAGE_BYTE(hdr), i = 0;
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
			case WT_ITEM_OFFPAGE:
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

/*
 * __wt_db_item_walk --
 *	Walk the items on a page and verify them.
 */
int
__wt_db_item_walk(WT_PAGE_HDR *hdr)
{
	return (0);
}
