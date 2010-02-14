/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_search_recno_col(
    WT_TOC *, u_int64_t, WT_PAGE **, WT_INDX **);
static int __wt_bt_search_recno_row(
    WT_TOC *, u_int64_t, WT_PAGE **, WT_INDX **);

/*
 * __wt_db_get_recno --
 *	Db.get_recno method.
 */
int
__wt_db_get_recno(
    DB *db, WT_TOC *toc, u_int64_t recno, DBT *key, DBT *pkey, DBT *data)
{
	IDB *idb;
	WT_INDX *indx;
	WT_PAGE *page;
	int ret;

	WT_ASSERT(toc->env, pkey == NULL);		/* NOT YET */

	idb = db->idb;

	WT_STAT_INCR(idb->stats, DB_READ_BY_RECNO);

	/* Check for a record past the end of the database. */
	if (idb->root_page->records < recno)
		return (WT_ERROR);

	/*
	 * Initialize the thread-of-control structure.
	 * We're willing to restart if the cache is too full.
	 */
	WT_TOC_DB_INIT(toc, db, "Db.get_recno");

	/* Search the primary btree for the key. */
	if (F_ISSET(idb, WT_COLUMN)) {
		WT_ERR(__wt_bt_search_recno_col(toc, recno, &page, &indx));
		ret = __wt_bt_dbt_return(toc, NULL, data, page, indx, 0);
	} else {
		WT_ERR(__wt_bt_search_recno_row(toc, recno, &page, &indx));
		ret = __wt_bt_dbt_return(toc, key, data, page, indx, 1);
	}

	/* Discard the returned page, if it's not the root page. */
	if (page != idb->root_page)
		WT_TRET(__wt_bt_page_out(toc, page, 0));

err:	WT_TOC_DB_CLEAR(toc);

	return (ret);
}

/*
 * __wt_bt_search_recno_var --
 *	Search a row store tree for a specific record-based key.
 */
static int
__wt_bt_search_recno_row(
    WT_TOC *toc, u_int64_t recno, WT_PAGE **pagep, WT_INDX **indxp)
{
	DB *db;
	IDB *idb;
	WT_INDX *ip;
	WT_PAGE *page;
	u_int64_t record_cnt;
	u_int32_t addr, i, type;
	int isleaf, next_isleaf;

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
			*indxp = ip = page->indx + ((recno - record_cnt) - 1);
			break;
		}

		/* Walk the page, counting records. */
		WT_INDX_FOREACH(page, ip, i) {
			if (record_cnt + WT_INDX_ITEM_OFF_RECORDS(ip) >= recno)
				break;
			record_cnt += WT_INDX_ITEM_OFF_RECORDS(ip);
		}

		/* ip references the subtree containing the record. */
		addr = WT_INDX_ITEM_OFF_ADDR(ip);
		next_isleaf =
		    WT_ITEM_TYPE(ip->page_data) == WT_ITEM_OFF_LEAF ? 1 : 0;

		/* We're done with the page. */
		if (page != idb->root_page)
			WT_RET(__wt_bt_page_out(toc, page, 0));

		isleaf = next_isleaf;

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

/*
 * __wt_bt_search_recno_col --
 *	Search a column store tree for a specific record-based key.
 */
static int
__wt_bt_search_recno_col(
    WT_TOC *toc, u_int64_t recno, WT_PAGE **pagep, WT_INDX **indxp)
{
	IDB *idb;
	WT_INDX *ip;
	WT_PAGE *page;
	u_int64_t record_cnt;
	u_int32_t addr, i;
	int isleaf, next_isleaf;

	idb = toc->db->idb;

	if ((page = idb->root_page) == NULL)
		return (WT_NOTFOUND);
	isleaf = page->hdr->type == WT_PAGE_COL_VAR ? 1 : 0;

	/* Search the tree. */
	for (record_cnt = 0;;) {
		/* If it's a leaf page, return the page and index. */
		if (isleaf) {
			*pagep = page;
			*indxp = page->indx + ((recno - record_cnt) - 1);
			return (0);
		}

		/* Walk the page, counting records. */
		WT_INDX_FOREACH(page, ip, i) {
			if (record_cnt + WT_INDX_OFF_RECORDS(ip) >= recno)
				break;
			record_cnt += WT_INDX_OFF_RECORDS(ip);
		}

		/* ip references the subtree containing the record. */
		addr = WT_INDX_OFF_ADDR(ip);
		next_isleaf = F_ISSET(page->hdr, WT_OFFPAGE_REF_LEAF) ? 1 : 0;

		/* We're done with the page. */
		if (page != idb->root_page)
			WT_RET(__wt_bt_page_out(toc, page, 0));

		isleaf = next_isleaf;

		/* Get the next page. */
		WT_RET(__wt_bt_page_in(toc, addr, isleaf, 1, &page));
	}
}
