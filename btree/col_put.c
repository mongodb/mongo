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
 * __wt_db_col_del --
 *	Db.col_del method.
 */
int
__wt_db_col_del(WT_TOC *toc, u_int64_t recno)
{
	ENV *env;
	IDB *idb;
	WT_PAGE *page;
	WT_REPL *new, *repl;
	WT_ROW_INDX *ip;
	int ret;

	env = toc->env;
	idb = toc->db->idb;
	page = NULL;
	ret = 0;

	/* Search the btree for the key. */
	WT_ERR(__wt_bt_search_col(toc, recno));
	page = toc->srch_page;
	ip = toc->srch_ip;

	/* Grow or allocate the replacement array if necessary. */
	repl = ip->repl;
	if (repl == NULL || repl->repl_next == repl->repl_size)
		WT_ERR(__wt_bt_repl_alloc(env, repl, &new));
	else
		new = NULL;

	/* Delete the item. */
	__wt_bt_del_serial(toc, new, ret);

err:	if (page != NULL && page != idb->root_page)
		WT_TRET(__wt_bt_page_out(toc, &page, 0));

	return (ret);
}
