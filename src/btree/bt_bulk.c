/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __bulk_row_keycmp_err(WT_CURSOR_BULK *);

/*
 * __wt_bulk_init --
 *	Start a bulk load.
 */
int
__wt_bulk_init(WT_CURSOR_BULK *cbulk)
{
	WT_SESSION_IMPL *session;
	int ret;

	session = (WT_SESSION_IMPL *)cbulk->cbt.iface.session;

	/*
	 * You can't bulk-load into existing trees.  Check, and retrieve the
	 * leaf page we're going to use.
	 */
	if ((ret = __wt_btree_root_empty(session, &cbulk->leaf)) != 0) {
		__wt_errx(
		    session, "bulk-load is only possible for empty trees");
		return (ret);
	}

	WT_RET(__wt_rec_bulk_init(cbulk));

	return (0);
}

/*
 * __wt_bulk_insert --
 *	Bulk insert, called once per item.
 */
int
__wt_bulk_insert(WT_CURSOR_BULK *cbulk)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_SESSION_IMPL *session;
	int cmp;

	session = (WT_SESSION_IMPL *)cbulk->cbt.iface.session;
	btree = session->btree;
	cursor = &cbulk->cbt.iface;

	switch (btree->type) {
	case BTREE_COL_FIX:
		WT_RET(__wt_rec_col_fix_bulk_insert(cbulk));
		break;
	case BTREE_COL_VAR:
		/*
		 * If this isn't the first value inserted, compare it against
		 * the last value and increment the RLE count.
		 *
		 * Instead of a "first time" variable, I'm using the RLE count,
		 * because it is set to 0 exactly once, the first time through
		 * the code.
		 */
		if (cbulk->rle != 0) {
			if (cbulk->cmp.size == cursor->value.size &&
			    memcmp(cbulk->cmp.data,
			    cursor->value.data, cursor->value.size) == 0) {
				++cbulk->rle;
				break;
			}
			WT_RET(__wt_rec_col_var_bulk_insert(cbulk));
		}
		WT_RET(__wt_buf_set(session,
		    &cbulk->cmp, cursor->value.data, cursor->value.size));
		cbulk->rle = 1;
		break;
	case BTREE_ROW:
		/*
		 * If this isn't the first value inserted, compare it against
		 * the last key to ensure the application doesn't accidentally
		 * corrupt the table.
		 *
		 * Instead of a "first time" variable, I'm using the RLE count,
		 * because it is set to 0 exactly once, the first time through
		 * the code.
		 */
		if (cbulk->rle != 0) {
			WT_RET(WT_BTREE_CMP(session, session->btree,
			    (WT_ITEM *)&cursor->key,
			    (WT_ITEM *)&cbulk->cmp, cmp));
			if (cmp <= 0)
				return (__bulk_row_keycmp_err(cbulk));
		}
		WT_RET(__wt_buf_set(session,
		    &cbulk->cmp, cursor->key.data, cursor->key.size));
		cbulk->rle = 1;

		WT_RET(__wt_rec_row_bulk_insert(cbulk));
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	WT_BSTAT_INCR(session, file_bulk_loaded);
	return (0);
}

/*
 * __wt_bulk_end --
 *	Clean up after a bulk load.
 */
int
__wt_bulk_end(WT_CURSOR_BULK *cbulk)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbulk->cbt.iface.session;

	WT_RET(__wt_rec_bulk_wrapup(cbulk));
	WT_RET(__wt_rec_evict(session, cbulk->leaf, WT_REC_SINGLE));

	__wt_buf_free(session, &cbulk->cmp);

	return (0);
}

/*
 * __bulk_row_keycmp_err --
 *	Error routine when keys inserted out-of-order.
 */
static int
__bulk_row_keycmp_err(WT_CURSOR_BULK *cbulk)
{
	WT_BUF a, b;
	WT_CURSOR *cursor;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbulk->cbt.iface.session;
	cursor = &cbulk->cbt.iface;

	WT_CLEAR(a);
	WT_CLEAR(b);

	WT_RET(__wt_buf_set_printable(
	    session, &a, cursor->key.data, cursor->key.size));
	WT_RET(__wt_buf_set_printable(
	    session, &b, cbulk->cmp.data, cbulk->cmp.size));

	__wt_errx( session,
	    "bulk-load presented with out-of-order keys: %.*s compares smaller "
	    "than previously inserted key %.*s",
	    (int)a.size, (char *)a.data, (int)b.size, (char *)b.data);
	return (WT_ERROR);
}
