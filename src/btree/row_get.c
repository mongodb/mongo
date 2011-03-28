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
	int ret;

	btree = session->btree;

	/* Search the btree for the key. */
	WT_RET(__wt_row_search(session, key, 0));

	ret = __wt_return_data(session, key, value, 0);

	if (session->srch_page != btree->root_page.page)
		__wt_hazard_clear(session, session->srch_page);

	return (ret);
}
