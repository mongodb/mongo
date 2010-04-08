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
__wt_bt_close(WT_TOC *toc)
{
	DB *db;
	ENV *env;
	IDB *idb;
	WT_PAGE *page;
	int isleaf, ret;

	env = toc->env;
	db = toc->db;
	idb = db->idb;
	ret = 0;

	/*
	 * Discard any pinned root page.  We have a direct reference but we
	 * have to get another one, otherwise the hazard pointers won't be
	 * set/cleared appropriately.
	 */
	if (idb->root_page != NULL) {
		isleaf = idb->root_page->addr == WT_ADDR_FIRST_PAGE ? 1 : 0;
		WT_TRET(__wt_bt_page_in(
		    toc, idb->root_page->addr, isleaf, 0, &page));
		F_CLR(page, WT_PINNED);
		WT_TRET(__wt_bt_page_out(toc, page, 0));
		idb->root_page = NULL;
	}

	/* Close the underlying file handle. */
	WT_TRET(__wt_close(env, idb->fh));
	idb->fh = NULL;

	return (ret);
}
