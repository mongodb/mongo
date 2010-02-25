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
__wt_bt_search_key_row(WT_TOC *toc, DBT *key, WT_SRCH *retp, u_int32_t flags)
{
	DB *db;
	ENV *env;
	IDB *idb;
	WT_BIN_INDX *bp;
	WT_PAGE *page;
	WT_ROW_INDX *ip;
	u_int32_t addr, base, indx, limit;
	int cmp, isleaf, ret;

	env = toc->env;
	db = toc->db;
	idb = db->idb;

	WT_ENV_FCHK(env,
	    "bt_search_key_row", flags, WT_APIMASK_BT_SEARCH_KEY_ROW);

	if ((page = idb->root_page) == NULL)
		return (WT_NOTFOUND);
	isleaf = page->hdr->type == WT_PAGE_ROW_LEAF ? 1 : 0;

	/* Search the tree. */
	for (;;) {
		/*
		 * There are two ways to search the tree; one is a binary search
		 * of the index array (created as the page is read into memory),
		 * the other walks a binary tree (created when a new element is
		 * inserted into the page).  Both loops need to be tight.
		 */
		if (page->bin == NULL) {
			for (base = 0,
			    limit = page->indx_count; limit != 0; limit >>= 1) {
				indx = base + (limit >> 1);

				/*
				 * If the key is compressed or an overflow, it
				 * may not have been instantiated yet.
				 */
				ip = page->u.r_indx + indx;
				if (WT_KEY_PROCESS(ip))
					WT_ERR(
					    __wt_bt_key_process(toc, ip, NULL));

				/*
				 * If we're about to compare an application key
				 * with the 0th index on an internal page,
				 * pretend the 0th index sorts less than any
				 * application key.  This test is so we don't
				 * have to update internal pages if the
				 * application stores a new, "smallest" key in
				 * the tree.
				 *
				 * For the record, we still maintain the key at
				 * the 0th location because it means tree
				 * verification and other code that processes
				 * a level of the tree doesn't need to know
				 * about this hack.
				 */
				if (indx != 0 || isleaf) {
					cmp = db->btree_compare(
					    db, key, (DBT *)ip);
					if (cmp == 0)
						break;
					if (cmp < 0)
						continue;
				}
				base = indx + 1;
				--limit;
			}

			/*
			 * Reference the slot used for next step down the tree.
			 * We do this on leaf pages too, because it's simpler
			 * to code, and we only care if there's an exact match
			 * on leaf pages; changing ip doesn't matter for leaf
			 * pages because we'll return WT_NOTFOUND if there's no
			 * match.
			 *
			 * Base is the smallest index greater than key and may
			 * be the 0th index or the (last + 1) indx.  If base
			 * is not the 0th index (remember, the 0th index always
			 * sorts less than any application key), decrement it
			 * to the smallest index less than or equal to key.
			 */
			if (cmp != 0)
				ip =
				    page->u.r_indx + (base == 0 ? 0 : base - 1);
		} else
			for (bp = page->bin;;) {
				/*
				 * If the key is compressed or an overflow, it
				 * may not have been instantiated yet.
				 */
				ip = bp->indx;
				if (WT_KEY_PROCESS(ip))
					WT_ERR(
					    __wt_bt_key_process(toc, ip, NULL));

				cmp = db->btree_compare(db, key, (DBT *)ip);
				if (cmp == 0)
					break;
				if (cmp < 0) {
					if (bp->left == NULL) {
						retp->bp = bp;
						retp->isleft = 1;
						break;
					}
					bp = bp->left;
				} else {
					if (bp->right == NULL) {
						retp->bp = bp;
						retp->isleft = 0;
						break;
					}
					bp = bp->right;
				}
			}

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
				retp->page = page;
				retp->indx = ip;
				return (cmp == 0 ? 0 : WT_NOTFOUND);
			}
			ret = WT_NOTFOUND;
			goto err;
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
__wt_bt_search_recno_row(WT_TOC *toc, u_int64_t recno, WT_SRCH *retp)
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
		__wt_api_db_errx(db,
		    "the Db.get_recno method cannot return keys with duplicate "
		    "data items; use the Db.cursor method instead");
		return (WT_ERROR);
	}

	retp->page = page;
	retp->indx = ip;
	return (0);
}
