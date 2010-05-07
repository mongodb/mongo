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
 * __wt_db_row_del --
 *	Db.row_del method.
 */
int
__wt_db_row_del(WT_TOC *toc, DBT *key)
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
	WT_ERR(__wt_bt_search_row(toc, key, 0));
	page = toc->srch_page;
	ip = toc->srch_ip;

	/* Grow or allocate the replacement array if necessary. */
	repl = ip->repl;
	if (repl == NULL || repl->repl_next == repl->repl_size)
		WT_ERR(__wt_bt_repl_alloc(env, repl, &new));
	else
		new = NULL;

	/* Delete the item. */
	__wt_bt_del_serial(toc, page, new, ret);

err:	if (page != NULL && page != idb->root_page)
		__wt_bt_page_out(toc, &page, ret == 0 ? WT_MODIFIED : 0);

	return (ret);
}

/*
 * __wt_bt_del_serial_func --
 *	Server function to discard an entry.
 */
int
__wt_bt_del_serial_func(WT_TOC *toc)
{
	WT_PAGE *page;
	WT_REPL *new, *repl;
	WT_ROW_INDX *ip;

	__wt_bt_del_unpack(toc, page, new);

	/* The entry we're updating is the last one pushed on the stack. */
	ip = toc->srch_ip;

	/*
	 * If our caller thought we'd need to install a new replacement array,
	 * check on that.
	 */
	if (new != NULL)
		WT_RET(__wt_workq_repl(toc, ip->repl, new, &ip->repl));

	/*
	 * Update the entry.  Incrementing the repl_next field makes this entry
	 * visible to the rest of the system; flush memory before incrementing
	 * it so it's never valid without supporting information.
	 */
	repl = ip->repl;
	repl->data[repl->repl_next].size = 0;
	repl->data[repl->repl_next].data = WT_DATA_DELETED;
	WT_MEMORY_FLUSH;
	++repl->repl_next;
	WT_MEMORY_FLUSH;
	page->modified = 1;

	return (0);
}
