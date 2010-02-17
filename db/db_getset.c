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

/*
 * __wt_db_column_set_verify --
 *	Verify arguments to the Db.set_column_set_verify setter.
 */
int
__wt_db_column_set_verify(DB *db,
    u_int32_t fixed_len, const char *dictionary, u_int32_t flags)
{
	IDB *idb;

	WT_CC_QUIET(dictionary, NULL);

	idb = db->idb;

	/* Repeat compression is incompatible with variable length records. */
	if (LF_ISSET(WT_REPEAT_COMP)) {
		if (fixed_len == 0) {
			__wt_db_errx(db,
			    "Repeat compression is incompatible with variable "
			    "length records");
			return (WT_ERROR);
		}
		F_SET(idb, WT_REPEAT_COMP);
	}

	/*
	 * We limit the size of fixed-length objects to 64 bytes, just so we
	 * don't have to deal with objects bigger than the page size.
	 */
	if (fixed_len > 64) {
		__wt_db_errx(db,
		    "Fixed-length objects are limited to 64 bytes in size");
		return (WT_ERROR);
	}

	/* Side-effect: this call means we're doing a column-store. */
	F_SET(idb, WT_COLUMN);
	return (0);
}
