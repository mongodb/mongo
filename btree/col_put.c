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
	int ret;

	env = toc->env;
	idb = toc->db->idb;

	/* Make sure we have a spare replacement array in the WT_TOC. */
	if (toc->repl_spare == NULL)
		WT_RET(__wt_calloc(
		    env, WT_REPL_CHUNK + 1, sizeof(WT_SDBT), &toc->repl_spare));

	/* Search the btree for the key. */
	WT_RET(__wt_bt_search_col(toc, recno));
	page = toc->srch_page;

	/* Delete the item. */
	__wt_bt_delete_serial(toc, page, ret);

	if (page != NULL && page != idb->root_page)
		__wt_bt_page_out(toc, &page, ret == 0 ? WT_MODIFIED : 0);

	return (0);
}
