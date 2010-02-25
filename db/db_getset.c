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
 *	Verify arguments to the Db.btree_compare_int_set method.
 */
int
__wt_db_btree_compare_int_set_verify(DB *db, int btree_compare_int)
{
	if (btree_compare_int >= 0 && btree_compare_int <= 8)
		return (0);

	__wt_api_db_errx(db,
	    "The number of bytes must be an integral value between 1 and 8");
	return (WT_ERROR);
}

/*
 * __wt_db_btree_dup_offpage_set_verify --
 *	Verify arguments to the Db.btree_dup_offpage_set method.
 */
int
__wt_db_btree_dup_offpage_set_verify(DB *db, u_int32_t dup_offpage)
{
	/*
	 * Limiting this value to something between 10 and 50 is a sanity test,
	 * not a hard constraint (although a value of 100 might fail hard).
	 *
	 * If the value is too large, pages can end up being empty because it
	 * isn't possible for duplicate sets to span pages.  So, if you set
	 * the value to 50%, and you have two sequential, large duplicate sets,
	 * you end up with two, half-empty pages.
	 */
	if (dup_offpage > 10 && dup_offpage <= 50)
		return (0);

	__wt_api_db_errx(db,
	    "The percent of the page taken up by duplicate entries before "
	    "being moved off-page must must be between 10 and 50");
	return (WT_ERROR);
}

/*
 * __wt_db_column_set_verify --
 *	Verify arguments to the Db.column_set method.
 */
int
__wt_db_column_set_verify(DB *db,
    u_int32_t fixed_len, const char *dictionary, u_int32_t flags)
{
	ENV *env;
	IDB *idb;

	env = db->env;
	idb = db->idb;

	/*
	 * The fixed-length number of bytes is stored in a single byte, which
	 * limits the size to 255 bytes.
	 */
	WT_RET(__wt_api_arg_max(
	    env, "DB.column_set", "fixed_len", fixed_len, 255));

	/*
	 * Dictionary and repeat compression are incompatible with variable
	 * length records.
	 */
	if (fixed_len == 0 &&
	    (dictionary != NULL || LF_ISSET(WT_REPEAT_COMP))) {
		__wt_api_db_errx(db,
		    "Repeated record count and dictionary compression are "
		    "incompatible with variable length column-store records");
		return (WT_ERROR);
	}

	if (LF_ISSET(WT_REPEAT_COMP))
		F_SET(idb, WT_REPEAT_COMP);
	F_SET(idb, WT_COLUMN);
	return (0);
}
