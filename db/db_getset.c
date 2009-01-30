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
__wt_db_set_btree_compare_int_verify(wt_args_db_set_btree_compare_int *argp)
{
	DB *db;

	db = argp->db;

	if (argp->btree_compare_int >= 0 && argp->btree_compare_int <= 8)
		return (0);

	__wt_db_errx(db,
	    "The number of bytes must be an integral value between 1 and 8");
	return (WT_ERROR);
}
