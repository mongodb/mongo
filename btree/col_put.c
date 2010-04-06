/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_del_serial_func(WT_TOC *);

/*
 * __wt_db_del --
 *	Db.del method.
 */
int
__wt_db_del(DB *db, WT_TOC *toc, DBT *key)
{
	ENV *env;
	IDB *idb;
	WT_PAGE *page;
	WT_REPL *new, *repl;
	WT_ROW_INDX *ip;
	int ret;

	env = db->env;
	idb = db->idb;
	page = NULL;
	ret = 0;

	WT_STAT_INCR(idb->stats, DB_DELETE_BY_KEY);

	WT_TOC_DB_INIT(toc, db, "Db.del");

	/* Search the btree for the key. */
	WT_ERR(__wt_bt_search_key_row(toc, key, 0));
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

err:	if (page != idb->root_page)
		WT_TRET(__wt_bt_page_out(toc, page, 0));

	WT_TOC_DB_CLEAR(toc);

	return (ret);
}

/*
 * __wt_bt_del_serial_func --
 *	Server function to discard an entry.
 */
static int
__wt_bt_del_serial_func(WT_TOC *toc)
{
	WT_REPL *new, *repl;
	WT_ROW_INDX *ip;

	__wt_bt_del_unpack(toc, new);

	/* The entry we're updating is the last one pushed on the stack. */
	ip = toc->srch_ip;

	/*
	 * If our caller thought we'd need to install a new replacement array,
	 * check on that.
	 */
	if (new != NULL)
		WT_RET(__wt_workq_repl(toc, ip->repl, new, &ip->repl));

	/*
	 * Update the entry.  The data field makes this entry visible to the
	 * rest of the system; flush memory before setting the data field so
	 * it is never valid without supporting information.
	 */
	repl = ip->repl;
	repl->data[repl->repl_next].size = 0;
	WT_MEMORY_FLUSH;
	repl->data[repl->repl_next].data = WT_DATA_DELETED;
	++repl->repl_next;
	WT_MEMORY_FLUSH;
	return (0);
}
