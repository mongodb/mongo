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
 * __wt_bt_close --
 *	Close a Btree.
 */
int
__wt_bt_close(DB *db)
{
	ENV *env;
	IDB *idb;
	WT_TOC *toc;
	WT_PAGE *page;
	int isleaf, ret;

	env = db->env;
	idb = db->idb;
	ret = 0;

	WT_RET(env->toc(env, 0, &toc));
	WT_TOC_DB_INIT(toc, db, "Db.close");

	/*
	 * Discard any pinned root page.  We have a direct reference but we
	 * have to get another one, otherwise the hazard pointers won't be
	 * set.
	 */
	if (idb->root_page != NULL) {
		isleaf = idb->root_page->addr == WT_ADDR_FIRST_PAGE ? 1 : 0;
		WT_TRET(__wt_bt_page_in(
		    toc, idb->root_page->addr, isleaf, 0, &page));
		F_CLR(page, WT_PINNED);
		WT_TRET(__wt_bt_page_out(toc, page, 0));
		idb->root_page = NULL;
	}

	/* Flush any modified pages, and discard the tree. */
	WT_TRET(__wt_bt_sync(db, NULL));

	/* Close the underlying file handle. */
	WT_TRET(__wt_close(env, idb->fh));
	idb->fh = NULL;

	WT_TRET(toc->close(toc, 0));

	return (ret);
}
