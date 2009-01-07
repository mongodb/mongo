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
 * __wt_bt_open --
 *	Open a Btree.
 */
int
__wt_bt_open(DB *db)
{
	IDB *idb;
	WT_PAGE_DESC desc;
	int ret;

	idb = db->idb;

	/* Open the underlying database file. */
	if ((ret = __wt_cache_db_open(db)) != 0)
		return (ret);

	/*
	 * Retrieve the root fragment address -- if the number of frags in
	 * the file is non-zero, there had better be a description record.
	 */
	if (idb->frags != 0) {
		if ((ret = __wt_bt_desc_read(db, &desc)) != 0)
			return (ret);
		idb->root_addr = desc.root_addr;
	} else
		idb->root_addr = WT_ADDR_INVALID;

	return (0);
}
