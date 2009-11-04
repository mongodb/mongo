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
 * __wt_db_btree_compare_int_set_verify --
 *	Verify arguments to the Db.set_btree_compare_int setter.
 */
int
__wt_db_btree_compare_int_set_verify(DB *db, int btree_compare_int)
{
	if (btree_compare_int >= 0 && btree_compare_int <= 8)
		return (0);

	__wt_db_errx(db,
	    "The number of bytes must be an integral value between 1 and 8");
	return (WT_ERROR);
}
