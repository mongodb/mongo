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
__wt_db_get_recno(DB *db, WT_TOC *toc, u_int64_t recno, DBT *data)
{
	IDB *idb;
	int ret;

	idb = db->idb;

	WT_STAT_INCR(idb->stats, DB_READ_BY_RECNO);

	/* Search the column store for the key. */
	if (F_ISSET(idb, WT_COLUMN)) {
		WT_TOC_DB_INIT(toc, db, "Db.get_recno");
		if ((ret = __wt_bt_search_recno_col(toc, recno)) == 0)
			ret = __wt_bt_dbt_return(
			    toc, NULL, data, toc->srch_page, toc->srch_ip, 0);
		WT_TOC_DB_CLEAR(toc);
		return (ret);
	}
	__wt_api_db_errx(db,
	    "row database records cannot be retrieved by record number");
	return (WT_ERROR);
}
