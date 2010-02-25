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
 * __wt_db_get_recno --
 *	Db.get_recno method.
 */
int
__wt_db_get_recno(
    DB *db, WT_TOC *toc, u_int64_t recno, DBT *key, DBT *pkey, DBT *data)
{
	IDB *idb;
	void *ip;
	WT_PAGE *page;
	WT_SRCH srch;
	int ret;

	WT_ASSERT(toc->env, pkey == NULL);		/* NOT YET */

	idb = db->idb;

	WT_STAT_INCR(idb->stats, DB_READ_BY_RECNO);

	/* Check for a record past the end of the database. */
	if (idb->root_page->records < recno)
		return (WT_NOTFOUND);

	/*
	 * Initialize the thread-of-control structure.
	 * We're willing to restart if the cache is too full.
	 */
	WT_TOC_DB_INIT(toc, db, "Db.get_recno");

	/* Search the primary btree for the key. */
	if (F_ISSET(idb, WT_COLUMN)) {
		WT_ERR(__wt_bt_search_recno_col(toc, recno, &page, &ip));
		ret = __wt_bt_dbt_return(toc, NULL, data, page, ip, 0);
	} else {
		WT_ERR(__wt_bt_search_recno_row(toc, recno, &srch));
		page = srch.page;
		ip = srch.indx;
		ret = __wt_bt_dbt_return(toc, key, data, page, ip, 1);
	}

	/* Discard the returned page, if it's not the root page. */
	if (page != idb->root_page)
		WT_TRET(__wt_bt_page_out(toc, page, 0));

err:	WT_TOC_DB_CLEAR(toc);

	return (ret);
}
