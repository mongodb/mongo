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
 * __wt_bt_search_key_row --
 *	Search a row-store tree for a specific key.
 */
int
__wt_bt_search_key_row(WT_TOC *toc, DBT *key, u_int32_t flags)
{
	DB *db;
	IDB *idb;
	WT_PAGE *page;
	WT_ROW_INDX *ip;
	u_int32_t addr, base, indx, limit;
	int cmp, isleaf, ret;

	db = toc->db;
	idb = db->idb;

	WT_DB_FCHK(db,
	    "__wt_bt_search_key_row", flags, WT_APIMASK_BT_SEARCH_KEY_ROW);

	if ((page = idb->root_page) == NULL)
		return (WT_NOTFOUND);
	isleaf = page->hdr->type == WT_PAGE_ROW_LEAF ? 1 : 0;

	/* Search the tree. */
	for (;;) {
		for (base = 0,
		    limit = page->indx_count; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);

			/*
			 * If the key is compressed or an overflow, it may not
			 * have been instantiated yet.
			 */
			ip = page->u.r_indx + indx;
			if (WT_KEY_PROCESS(ip))
				WT_ERR(__wt_bt_key_process(toc, ip, NULL));

			/*
			 * If we're about to compare an application key with the
			 * 0th index on an internal page, pretend the 0th index
			 * sorts less than any application key.  This test is so
			 * we don't have to update internal pages if the
			 * application stores a new, "smallest" key in the tree.
			 *
			 * For the record, we still maintain the key at the 0th
			 * location because it means tree verification and other
			 * code that processes a level of the tree doesn't need
			 * to know about this hack.
			 */
			if (indx != 0 || isleaf) {
				cmp = db->btree_compare(db, key, (DBT *)ip);
				if (cmp == 0)
					break;
				if (cmp < 0)
					continue;
			}
			base = indx + 1;
			--limit;
		}

		/*
		 * Reference the slot used for next step down the tree.  We do
		 * this on leaf pages too, because it's simpler to code, and we
		 * only care if there's an exact match on leaf pages; setting
		 * ip doesn't matter for leaf pages because we always return
		 * WT_NOTFOUND if there's no match.
		 *
		 * Base is the smallest index greater than key and may be the
		 * 0th index or the (last + 1) indx.  If base is not the 0th
		 * index (remember, the 0th index always sorts less than any
		 * application key), decrement it to the smallest index less
		 * than or equal to key.
		 */
		if (cmp != 0)
			ip = page->u.r_indx + (base == 0 ? 0 : base - 1);

		/*
		 * If we've reached the leaf page:
		 *	If we found a match, set the page/index and return OK.
		 *	If we didn't find a match, but we're inserting, set
		 *	    the page/index and return WT_NOTFOUND.
		 *	If we didn't find a match and it's not an insert, lose
		 *	    the page and return WT_NOTFOUND.
		 */
		if (isleaf) {
			if (cmp == 0 || LF_ISSET(WT_INSERT)) {
				toc->srch_page = page;
				toc->srch_ip = ip;
				if (cmp == 0)
					return (0);
			}
			return (WT_NOTFOUND);
		}

		/* Get the address for the child page. */
		addr = WT_ROW_OFF_ADDR(ip);
		isleaf =
		    WT_ITEM_TYPE(ip->page_data) == WT_ITEM_OFF_LEAF ? 1 : 0;

		/* We're done with the page. */
		if (page != idb->root_page)
			WT_RET(__wt_bt_page_out(toc, page, 0));

		/* Get the next page. */
		WT_RET(__wt_bt_page_in(toc, addr, isleaf, 1, &page));
	}

err:	if (page != idb->root_page)
		WT_RET(__wt_bt_page_out(toc, page, 0));
	return (ret);
}
