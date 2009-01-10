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
 * __wt_db_set_btree_compare_int_verify --
 *	Verify arguments to the Db.set_btree_compare_int setter.
 */
int
__wt_db_set_btree_compare_int_verify(DB *db, int *bytesp)
{
	int bytes;

	bytes = *bytesp;
	if (bytes >= 0 && bytes <= 8) {
		db->btree_compare = __wt_bt_int_compare;
		return (0);
	}

	__wt_db_errx(db,
	    "The number of bytes must be an integral value between 1 and 8");
	return (WT_ERROR);
}
