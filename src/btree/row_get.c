/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_btree_row_get --
 *	Db.row_get method.
 */
int
__wt_btree_row_get(SESSION *session, WT_ITEM *key, WT_ITEM *value)
{
	BTREE *btree;
	WT_PAGE *page;
	int ret;

	btree = session->btree;
	page = NULL;

	/* Search the btree for the key. */
	WT_ERR(__wt_row_search(session, key, 0));
	page = session->srch_page;

	ret = __wt_value_return(session, key, value, 0);

err:	if (page != btree->root_page.page)
		__wt_hazard_clear(session, page);
	return (ret);
}
