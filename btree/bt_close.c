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
	int ret;

	env = db->env;
	idb = db->idb;
	ret = 0;

	/* Flush any modified pages, and discard the tree. */
	WT_TRET(__wt_bt_sync(db));
	idb->root_page = NULL;
	idb->root_addr = WT_ADDR_INVALID;

	/* Close the underlying file handle. */
	WT_TRET(__wt_close(env, idb->fh));
	idb->fh = NULL;

	return (ret);
}
