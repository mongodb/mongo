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
	int ret;

	env = db->env;
	idb = db->idb;
	ret = 0;

	WT_RET(env->toc(env, 0, &toc));
	WT_TOC_DB_INIT(toc, db, "Db.close");

	/* Discard the pinned root page. */
	if (idb->root_page != NULL) {
		F_CLR(idb->root_page, WT_PINNED);
		WT_TRET(__wt_bt_page_out(toc, idb->root_page, 0));
		idb->root_page = NULL;
	}
	idb->root_addr = WT_ADDR_INVALID;

	/* Flush any modified pages, and discard the tree. */
	WT_TRET(__wt_bt_sync(db, NULL));

	/* Close the underlying file handle. */
	WT_TRET(__wt_close(env, idb->fh));
	idb->fh = NULL;

	WT_TRET(toc->close(toc, 0));

	return (ret);
}
