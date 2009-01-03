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
	int i, ret;

	idb = db->idb;

	/* Initialize the page queues. */
	for (i = 0; i < WT_HASHSIZE; ++i)
		TAILQ_INIT(&idb->hqh[i]);
	TAILQ_INIT(&idb->lqh);

	/* Open the underlying database file. */
	if ((ret = __wt_db_page_open(idb)) != 0)
		return (ret);

	/*
	 * Retrieve the root fragment address -- if the number of frags in
	 * the file is non-zero, there had better be a description record.
	 */
	if (idb->frags != 0) {
		if ((ret = __wt_db_desc_read(db, &desc)) != 0)
			return (ret);
		idb->root_addr = desc.root_addr;
	} else
		idb->root_addr = WT_ADDR_INVALID;

	return (0);
}
