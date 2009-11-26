/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_search(WT_TOC *, DBT *, WT_PAGE **, WT_INDX **);
static int __wt_bt_search_recno(WT_TOC *, u_int64_t, WT_PAGE **, WT_INDX **);

/*
 * __wt_db_get --
 *	Db.get method when called directly from a user thread.
 */
int
__wt_db_get(
    DB *db, WT_TOC *toc, DBT *key, DBT *pkey, DBT *data, u_int32_t flags)
{
	IDB *idb;
	WT_INDX *indx;
	WT_PAGE *page;
	u_int32_t type;
	int ret;

	WT_ASSERT(toc->env, pkey == NULL);		/* NOT YET */

	idb = db->idb;

	WT_STAT_INCR(idb->stats,
	    DB_READ_BY_KEY, "database read-by-key operations");

	/* Initialize the thread-of-control structure. */
	WT_TOC_DB_INIT(toc, db, "Db.get");

	/* Search the primary btree for the key. */
	WT_RET(__wt_bt_search(toc, key, &page, &indx));

	/*
	 * The Db.get method can only return single key/data pairs.
	 * If that's not what we found, we're done.
	 */
	type = WT_ITEM_TYPE(indx->ditem);
	if (type != WT_ITEM_DATA && type != WT_ITEM_DATA_OVFL) {
		__wt_db_errx(db,
		    "the Db.get method cannot return keys with duplicate "
		    "data items; use the Db.cursor method instead");
		ret = WT_ERROR;
	} else
		ret = __wt_bt_dbt_return(toc, key, data, page, indx, 0);

	/* Discard any page other than the root page, which remains pinned. */
	if (page != idb->root_page)
		WT_TRET(__wt_bt_page_out(toc, page, 0));

	WT_TOC_DB_CLEAR(toc);

	return (ret);
}

/*
 * __wt_bt_search --
 *	Search the tree for a specific key.
 */
static int
__wt_bt_search(WT_TOC *toc, DBT *key, WT_PAGE **pagep, WT_INDX **indxp)
{
	DB *db;
	IDB *idb;
	WT_INDX *ip;
	WT_PAGE *page;
	u_int32_t addr, base, indx, limit;
	int cmp, isleaf, next_isleaf, put_page, ret;

	db = toc->db;
	idb = db->idb;

	if ((addr = idb->root_addr) == WT_ADDR_INVALID)
		return (WT_NOTFOUND);
	page = idb->root_page;
	isleaf = page->hdr->type == WT_PAGE_LEAF ? 1 : 0;

	/* Search the tree. */
	for (put_page = 0;; put_page = 1) {
		/*
		 * Do a binary search of the page -- this loop must be tight.
		 */
		for (base = 0,
		    limit = page->indx_count; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);

			/*
			 * If the key is an overflow, it may not have been
			 * instantiated yet.
			 */
			ip = page->indx + indx;
			if (ip->data == NULL)
				WT_ERR(__wt_bt_ovfl_to_indx(toc, page, ip));

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
				*indxp = ip;
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
			ip = page->indx + (base == 0 ? base : base - 1);
		}
		addr = WT_INDX_OFFP_ADDR(ip);
		next_isleaf =
		    WT_ITEM_TYPE(ip->ditem) == WT_ITEM_OFFP_LEAF ? 1 : 0;

		/* We're done with the page. */
		if (put_page)
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
err:	if (put_page)
		(void)__wt_bt_page_out(toc, page, 0);
	return (ret);
}

/*
 * __wt_db_get_recno --
 *	Db.get_recno method when called directly from a user thread.
 */
int
__wt_db_get_recno(DB *db, WT_TOC *toc,
    u_int64_t recno, DBT *key, DBT *pkey, DBT *data, u_int32_t flags)
{
	IDB *idb;
	WT_INDX *indx;
	WT_PAGE *page;
	u_int32_t type;
	int ret;

	WT_ASSERT(toc->env, pkey == NULL);		/* NOT YET */

	idb = db->idb;

	WT_STAT_INCR(idb->stats,
	    DB_READ_BY_RECNO, "database read-by-recno operations");

	/* Check for a record past the end of the database. */
	if (idb->root_page->records < recno)
		return (WT_ERROR);

	/* Initialize the thread-of-control structure. */
	WT_TOC_DB_INIT(toc, db, "Db.get_recno");

	/* Search the primary btree for the key. */
	WT_RET(__wt_bt_search_recno(toc, recno, &page, &indx));

	/*
	 * The Db.get_recno method can only return single key/data pairs.
	 * If that's not what we found, we're done.
	 */
	type = WT_ITEM_TYPE(indx->ditem);
	if (type != WT_ITEM_DATA && type != WT_ITEM_DATA_OVFL) {
		__wt_db_errx(db,
		    "the Db.get_recno method cannot return keys with duplicate "
		    "data items; use the Db.cursor method instead");
		ret = WT_ERROR;
	} else
		ret = __wt_bt_dbt_return(toc, key, data, page, indx, 1);

	/* Discard any page other than the root page, which remains pinned. */
	if (page != idb->root_page)
		WT_TRET(__wt_bt_page_out(toc, page, 0));

	WT_TOC_DB_CLEAR(toc);

	return (ret);
}

/*
 * __wt_bt_search_recno --
 *	Search the tree for a specific record-based key.
 */
static int
__wt_bt_search_recno(
    WT_TOC *toc, u_int64_t recno, WT_PAGE **pagep, WT_INDX **indxp)
{
	IDB *idb;
	WT_INDX *ip;
	WT_PAGE *page;
	u_int64_t record_cnt;
	u_int32_t addr, i;
	int isleaf, next_isleaf, put_page;

	idb = toc->db->idb;

	if ((addr = idb->root_addr) == WT_ADDR_INVALID)
		return (WT_NOTFOUND);
	page = idb->root_page;
	record_cnt = 0;
	isleaf = page->hdr->type == WT_PAGE_LEAF ? 1 : 0;

	/* Search the tree. */
	for (put_page = 0;; put_page = 1) {
		/* If it's a leaf page, return the page and index. */
		if (isleaf) {
			*pagep = page;
			*indxp = page->indx + ((recno - record_cnt) - 1);
			return (0);
		}

		/* Walk the page, counting records. */
		WT_INDX_FOREACH(page, ip, i) {
			if (record_cnt + WT_INDX_OFFP_RECORDS(ip) >= recno)
				break;
			record_cnt += WT_INDX_OFFP_RECORDS(ip);
		}

		/* ip references the subtree containing the record. */
		addr = WT_INDX_OFFP_ADDR(ip);
		next_isleaf =
		    WT_ITEM_TYPE(ip->ditem) == WT_ITEM_OFFP_LEAF ? 1 : 0;

		/* We're done with the page. */
		if (put_page)
			WT_RET(__wt_bt_page_out(toc, page, 0));

		isleaf = next_isleaf;

		/* Get the next page. */
		WT_RET(__wt_bt_page_in(toc, addr, isleaf, 1, &page));
	}
}
