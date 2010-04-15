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
 * __wt_bt_search_row --
 *	Search a row-store tree for a specific key.
 */
int
__wt_bt_search_row(WT_TOC *toc, DBT *key, u_int32_t flags)
{
	DB *db;
	IDB *idb;
	WT_PAGE *page;
	WT_ROW_INDX *ip;
	WT_SDBT *rdbt;
	u_int32_t addr, base, indx, limit, size;
	int cmp, isleaf, ret;

	toc->srch_page = NULL;			/* Return values. */
	toc->srch_ip = NULL;

	db = toc->db;
	idb = db->idb;

	WT_DB_FCHK(db,
	    "__wt_bt_search_key_row", flags, WT_APIMASK_BT_SEARCH_KEY_ROW);

	if (WT_UNOPENED_DATABASE(idb))
		return (WT_NOTFOUND);

	/* Search the tree. */
	for (page = idb->root_page;;) {
		isleaf = page->hdr->type == WT_PAGE_ROW_LEAF;
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

		/* If we've reached the leaf page, we're done. */
		if (isleaf)
			break;

		/* Get the address for the child page. */
		addr = WT_ROW_OFF_ADDR(ip);
		size = WT_ROW_OFF_SIZE(ip);

		/* Walk down to the next page. */
		if (page != idb->root_page)
			WT_RET(__wt_bt_page_out(toc, &page, 0));
		WT_RET(__wt_bt_page_in(toc, addr, size, 1, &page));
	}

	/*
	 * If we're inserting, we're returning a position in the tree rather
	 * than an item, so it's always useful.
	 */
	if (!LF_ISSET(WT_INSERT)) {
		/* Lookups only return exact matches. */
		if (cmp != 0) {
			ret = WT_NOTFOUND;
			goto err;
		}

		/* Return the match unless it's been deleted. */
		if ((rdbt = WT_REPL_CURRENT(ip)) != NULL &&
		     rdbt->data == WT_DATA_DELETED) {
			ret = WT_NOTFOUND;
			goto err;
		}
	}
	toc->srch_page = page;
	toc->srch_ip = ip;
	return (0);

err:	if (page != idb->root_page)
		WT_RET(__wt_bt_page_out(toc, &page, 0));
	return (ret);
}
