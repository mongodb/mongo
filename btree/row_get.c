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
 * __wt_db_get --
 *	Db.get method.
 */
int
__wt_db_get(DB *db, WT_TOC *toc, DBT *key, DBT *pkey, DBT *data)
{
	IDB *idb;
	WT_PAGE *page;
	WT_ROW_INDX *ip;
	u_int32_t type;
	int ret;

	WT_ASSERT(toc->env, pkey == NULL);		/* NOT YET */

	idb = db->idb;

	WT_STAT_INCR(idb->stats, DB_READ_BY_KEY);

	/*
	 * Initialize the thread-of-control structure.
	 * We're will to re-start if the cache is too full.
	 */
	WT_TOC_DB_INIT(toc, db, "Db.get");

	/* Search the btree for the key. */
	WT_ERR(__wt_bt_search(toc, key, &page, &ip));

	/*
	 * The Db.get method can only return single key/data pairs.
	 * If that's not what we found, we're done.
	 */
	type = WT_ITEM_TYPE(ip->page_data);
	if (type != WT_ITEM_DATA && type != WT_ITEM_DATA_OVFL) {
		__wt_db_errx(db,
		    "the Db.get method cannot return keys with duplicate "
		    "data items; use the Db.cursor method instead");
		ret = WT_ERROR;
	} else
		ret = __wt_bt_dbt_return(toc, key, data, page, ip, 0);

	/* Discard the page. */
	if (page != idb->root_page)
		WT_TRET(__wt_bt_page_out(toc, page, 0));

err:	WT_TOC_DB_CLEAR(toc);

	return (ret);
}
