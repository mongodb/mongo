/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_btree_btree_compare_int_set_verify --
 *	Verify arguments to the Db.btree_compare_int_set method.
 */
int
__wt_btree_btree_compare_int_set_verify(BTREE *btree, int btree_compare_int)
{
	if (btree_compare_int >= 0 && btree_compare_int <= 8)
		return (0);

	__wt_errx(&btree->conn->default_session,
	    "The number of bytes must be an integral value between 1 and 8");
	return (WT_ERROR);
}

/*
 * __wt_btree_column_set_verify --
 *	Verify arguments to the Db.column_set method.
 */
int
__wt_btree_column_set_verify(
    BTREE *btree, uint32_t fixed_len, const char *dictionary, uint32_t flags)
{
	SESSION *session;

	WT_UNUSED(dictionary);

	session = &btree->conn->default_session;

	/*
	 * The fixed-length number of bytes is stored in a single byte, which
	 * limits the size to 255 bytes.
	 */
	WT_RET(__wt_api_arg_max(
	    session, "BTREE.column_set", "fixed_len", fixed_len, 255));

	/* Run-length encoding is incompatible with variable length records. */
	if (fixed_len == 0 && LF_ISSET(WT_RLE)) {
		__wt_errx(session,
		    "Run-length encoding is incompatible with variable length "
		    "column-store records");
		return (WT_ERROR);
	}

	if (LF_ISSET(WT_RLE))
		F_SET(btree, WT_RLE);
	F_SET(btree, WT_COLUMN);
	return (0);
}
