/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_search_recno(WT_TOC *, u_int64_t, WT_PAGE **, WT_INDX **);

/*
 * __wt_db_get_recno --
 *	Db.get_recno method.
 */
int
__wt_db_get_recno(DB *db, WT_TOC *toc,
    u_int64_t recno, DBT *key, DBT *pkey, DBT *data, u_int32_t flags)
{
	IDB *idb;
	IENV *ienv;
	WT_INDX *indx;
	WT_PAGE *page;
	u_int32_t type;
	int ret;

	WT_ASSERT(toc->env, pkey == NULL);		/* NOT YET */

	idb = db->idb;
	ienv = db->env->ienv;

	WT_STAT_INCR(idb->stats,
	    DB_READ_BY_RECNO, "database read-by-recno operations");

	/* Check for a record past the end of the database. */
	if (idb->root_page->records < recno)
		return (WT_ERROR);

	/*
	 * Initialize the thread-of-control structure.
	 * We're will to re-start if the cache is too full.
	 */
	WT_TOC_DB_INIT(toc, db, "Db.get_recno");

	/* Search the primary btree for the key. */
	F_SET(toc, WT_CACHE_LOCK_RESTART);
	while ((ret =
	    __wt_bt_search_recno(toc, recno, &page, &indx)) == WT_RESTART)
		WT_TOC_SERIALIZE_VALUE(toc, &ienv->cache_lockout);
	F_CLR(toc, WT_CACHE_LOCK_RESTART);
	if (ret != 0)
		goto err;

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

err:	WT_TOC_DB_CLEAR(toc);

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
