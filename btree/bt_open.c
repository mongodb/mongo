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
	IENV *ienv;
	int i, ret;

	ienv = db->ienv;
	idb = db->idb;

	TAILQ_INIT(&idb->hlru);
	for (i = 0; i < WT_HASHSIZE; ++i)
		TAILQ_INIT(&idb->hhq[i]);

	/* Open the underlying database file. */
	if ((ret = __wt_db_page_open(idb)) != 0)
		return (ret);

	return (0);
}
