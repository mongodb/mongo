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

	/* Search the column-store for the key. */
	if (!F_ISSET(btree, WT_COLUMN)) {
		__wt_errx(session,
		    "row-store records cannot be retrieved by record number");
		return (WT_ERROR);
	}

	WT_RET(__wt_col_search(session, recno, 0));

	ret = __wt_value_return(session, NULL, value, 0);

	if (session->srch_page != btree->root_page.page)
		__wt_hazard_clear(session, session->srch_page);
	return (ret);
}
