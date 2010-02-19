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
 * __wt_bt_search --
 *	Search a row-store tree for a specific key.
 */
int
__wt_bt_search(WT_TOC *toc, DBT *key, WT_PAGE **pagep, WT_ROW_INDX **ipp)
{
	DB *db;
	IDB *idb;
	WT_PAGE *page;
	WT_ROW_INDX *ip;
	u_int32_t addr, base, indx, limit;
	int cmp, isleaf, next_isleaf, ret;

	db = toc->db;
	idb = db->idb;

	if ((page = idb->root_page) == NULL)
		return (WT_NOTFOUND);
	isleaf = page->hdr->type == WT_PAGE_ROW_LEAF ? 1 : 0;

	/* Search the tree. */
	for (;;) {
		/*
		 * Do a binary search of the page -- this loop must be tight.
		 */
		for (base = 0,
		    limit = page->indx_count; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);

			/*
			 * If the key is compressed or an overflow, it may not
			 * have been instantiated yet.
			 */
			ip = page->u.r_indx + indx;
			if (WT_ROW_INDX_PROCESS(ip))
				WT_ERR(__wt_bt_key_to_indx(toc, ip));

			/*
			 * If we're about to compare an application key with
			 * the 0th index on an internal page, pretend the 0th
			 * index sorts less than any application key.  This
			 * test is so we don't have to update internal pages
			 * if the application stores a new, "smallest" key in
			 * the tree.
			 *
			 * For the record, we still maintain the key at the
			 * 0th location because it means tree verification
			 * and other code that processes a level of the tree
			 * doesn't need to know about this hack.
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
		 * If matched on a leaf page, return the page and the matching
		 * index.
		 *
		 * If matched on an internal page, continue searching down the
		 * tree from indx.
		 */
		if (cmp == 0) {
			if (isleaf) {
				*pagep = page;
				*ipp = ip;
				return (0);
			}
		} else {
			/*
			 * Base is the smallest index greater than key and may
			 * be the 0th index or the (last + 1) indx.  If base
			 * is not the 0th index (remember, the 0th index always
			 * sorts less than any application key), decrement it
			 * to the smallest index less than or equal to key.
			 */
			ip = page->u.r_indx + (base == 0 ? base : base - 1);
		}
		addr = WT_ROW_OFF_ADDR(ip);
		next_isleaf =
		    WT_ITEM_TYPE(ip->page_data) == WT_ITEM_OFF_LEAF ? 1 : 0;

		/* We're done with the page. */
		if (page != idb->root_page)
			WT_RET(__wt_bt_page_out(toc, page, 0));

		/*
		 * Failed to match on a leaf page -- we're done, return the
		 * failure.
		 */
		if (isleaf)
			return (WT_NOTFOUND);
		isleaf = next_isleaf;

		/* Get the next page. */
		WT_RET(__wt_bt_page_in(toc, addr, isleaf, 1, &page));
	}
	/* NOTREACHED */

	/* Discard any page we've read other than the root page. */
err:	if (page != idb->root_page)
		(void)__wt_bt_page_out(toc, page, 0);
	return (ret);
}

/*
 * __wt_bt_search_recno_row --
 *	Search a row-store tree for a specific record-based key.
 */
int
__wt_bt_search_recno_row(
    WT_TOC *toc, u_int64_t recno, WT_PAGE **pagep, void *ipp)
{
	DB *db;
	IDB *idb;
	WT_ROW_INDX *ip;
	WT_PAGE *page;
	u_int64_t record_cnt;
	u_int32_t addr, i, type;
	int isleaf;

	db = toc->db;
	idb = db->idb;

	if ((page = idb->root_page) == NULL)
		return (WT_NOTFOUND);
	isleaf = page->hdr->type == WT_PAGE_ROW_LEAF ? 1 : 0;

	/* Search the tree. */
	for (record_cnt = 0;;) {
		/* If it's a leaf page, return the page and index. */
		if (isleaf) {
			*pagep = page;
			*(WT_ROW_INDX **)ipp =
			    ip = page->u.r_indx + ((recno - record_cnt) - 1);
			break;
		}

		/* Walk the page, counting records. */
		WT_INDX_FOREACH(page, ip, i) {
			if (record_cnt + WT_ROW_OFF_RECORDS(ip) >= recno)
				break;
			record_cnt += WT_ROW_OFF_RECORDS(ip);
		}

		/* ip references the subtree containing the record. */
		addr = WT_ROW_OFF_ADDR(ip);
		isleaf =
		    WT_ITEM_TYPE(ip->page_data) == WT_ITEM_OFF_LEAF ? 1 : 0;

		/* We're done with the page. */
		if (page != idb->root_page)
			WT_RET(__wt_bt_page_out(toc, page, 0));

		/* Get the next page. */
		WT_RET(__wt_bt_page_in(toc, addr, isleaf, 1, &page));
	}

	/*
	 * The Db.get_recno method can only return single key/data pairs.
	 * If that's not what we found, we're done.
	 */
	type = WT_ITEM_TYPE(ip->page_data);
	if (type != WT_ITEM_DATA && type != WT_ITEM_DATA_OVFL) {
		__wt_db_errx(db,
		    "the Db.get_recno method cannot return keys with duplicate "
		    "data items; use the Db.cursor method instead");
		return (WT_ERROR);
	}
	return (0);
}
