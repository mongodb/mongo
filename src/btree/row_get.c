/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_db_row_get --
 *	Db.row_get method.
 */
int
__wt_db_row_get(WT_TOC *toc, DBT *key, DBT *data)
{
	IDB *idb;
	WT_PAGE *page;
	int ret;

	idb = toc->db->idb;
	page = NULL;

	/* Search the btree for the key. */
	WT_ERR(__wt_row_search(toc, key, WT_NOLEVEL, 0));
	page = toc->srch_page;

	ret = __wt_dbt_return(toc, key, data, 0);

err:	if (page != idb->root_page.page)
		__wt_hazard_clear(toc, page);
	return (ret);
}
