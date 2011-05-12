/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_btree_col_get --
 *	Db.col_get method.
 */
int
__wt_btree_col_get(SESSION *session, uint64_t recno, WT_ITEM *value)
{
	BTREE *btree;
	int ret;

	btree = session->btree;

	WT_RET(__wt_col_search(session, recno, 0));
	ret = __wt_return_data(session, NULL, value, 0);

	if (session->srch_page != btree->root_page.page)
		__wt_hazard_clear(session, session->srch_page);
	return (ret);
}
