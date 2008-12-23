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
	WT_BTREE *bt;
	IENV *ienv;
	int i, ret;

	ienv = db->ienv;
	idb = db->idb;
	bt = NULL;

	/* Allocate the Btree structure. */
	if ((ret = __wt_calloc(ienv, 1, sizeof(WT_BTREE), &bt)) != 0)
		return (ret);
	bt->db = db;

	TAILQ_INIT(&bt->hlru);
	for (i = 0; i < WT_HASHSIZE; ++i)
		TAILQ_INIT(&bt->hhq[i]);

	/* Open the underlying database file. */
	if ((ret = __wt_db_page_open(bt)) != 0)
		goto err;

	idb->btree = bt;
	return (0);

err:	if (bt != NULL)
		__wt_free(ienv, bt);
	return (ret);
}
