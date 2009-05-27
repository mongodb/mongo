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
	IDB *idb;
	int ret, tret;

	idb = db->idb;
	ret = 0;

	/* Discard any root page we've pinned. */
	if (idb->root_page != NULL) {
		ret = __wt_bt_page_out(db, STOC_PRIME, idb->root_page, 0);
		idb->root_page = NULL;
	}

	/* Close the underlying database file. */
	if ((tret = __wt_cache_db_close(db, STOC_PRIME)) != 0 && ret == 0)
		ret = tret;

	return (ret);
}
