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
__wt_bt_close(WT_STOC *stoc)
{
	IDB *idb;
	int ret;

	idb = stoc->db->idb;
	ret = 0;

	/* Discard any root page we've pinned. */
	if (idb->root_page != NULL) {
		ret = __wt_bt_page_out(stoc, idb->root_page, 0);
		idb->root_page = NULL;
	}

	return (ret);
}
