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
 * __wt_page_inmem --
 *	Initialize in-memory page structure
 */
void
__wt_page_inmem(DB *db, WT_PAGE *page, int is_alloc)
{
	WT_ITEM *item;
	u_int32_t i;

	if (is_alloc) {
		/*
		 * Initialize a WT_PAGE structure for a newly created database.
		 *
		 * Fragment 0 has a special property -- the first 64 bytes
		 * past the header holds the database's meta-data information.
		 */
		page->first_free = page->first_data = WT_PAGE_BYTE(page);
		page->space_avail = db->pagesize -
		    (u_int32_t)(page->first_free - (u_int8_t *)page->hdr);

		if (page->addr == 0)
			__wt_db_desc_init(db, page);
	} else if (page->hdr->type == WT_PAGE_OVFL)
			return;
	else {
		/*
		 * Walk the page looking for the end of the page.  Once we have
		 * the first free byte on the page we can figure out how much
		 * space is available.
		 */
		page->first_data = WT_PAGE_BYTE(page);
		for (item = (WT_ITEM *)page->first_data,
		    i = page->hdr->u.entries;
		    i > 0;
		    item = (WT_ITEM *)
		    ((u_int8_t *)item + WT_ITEM_SPACE_REQ(item->len)), --i)
			;
		page->first_free = (u_int8_t *)item;
		page->space_avail = db->pagesize -
		    (u_int32_t)(page->first_free - (u_int8_t *)page->hdr);
	}
}
