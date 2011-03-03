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
__wt_db_row_get(WT_TOC *toc, WT_DATAITEM *key, WT_DATAITEM *data)
{
	BTREE *btree;
	WT_PAGE *page;
	int ret;

	btree = toc->db->btree;
	page = NULL;

	/* Search the btree for the key. */
	WT_ERR(__wt_row_search(toc, key, 0));
	page = toc->srch_page;

	ret = __wt_data_return(toc, key, data, 0);

err:	if (page != btree->root_page.page)
		__wt_hazard_clear(toc, page);
	return (ret);
}
