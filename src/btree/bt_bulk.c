/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
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
	WT_BTREE *btree;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbulk->cbt.iface.session;
	btree = S2BT(session);

	/*
	 * Bulk-load is only permitted on newly created files, not any empty
	 * file -- see the checkpoint code for a discussion.
	 */
	if (!btree->bulk_load_ok)
		WT_RET_MSG(session, EINVAL,
		    "bulk-load is only possible for newly created trees");

	/* Set a reference to the empty leaf page. */
	cbulk->leaf = btree->root_page->u.intl.t->page;

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
	btree = S2BT(session);
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
			WT_RET(WT_BTREE_CMP(session, S2BT(session),
			    &cursor->key, &cbulk->cmp, cmp));
			if (cmp <= 0)
				return (__bulk_row_keycmp_err(cbulk));
		}
		WT_RET(__wt_buf_set(session,
		    &cbulk->cmp, cursor->key.data, cursor->key.size));
		cbulk->rle = 1;

		WT_RET(__wt_rec_row_bulk_insert(cbulk));
		break;
	WT_ILLEGAL_VALUE(session);
	}

	WT_DSTAT_INCR(session, cursor_insert_bulk);
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
	WT_CURSOR *cursor;
	WT_DECL_ITEM(a);
	WT_DECL_ITEM(b);
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbulk->cbt.iface.session;
	cursor = &cbulk->cbt.iface;

	WT_ERR(__wt_scr_alloc(session, 512, &a));
	WT_ERR(__wt_scr_alloc(session, 512, &b));

	WT_ERR(__wt_buf_set_printable(
	    session, a, cursor->key.data, cursor->key.size));
	WT_ERR(__wt_buf_set_printable(
	    session, b, cbulk->cmp.data, cbulk->cmp.size));

	WT_ERR_MSG(session, EINVAL,
	    "bulk-load presented with out-of-order keys: %.*s compares smaller "
	    "than previously inserted key %.*s",
	    (int)a->size, (const char *)a->data,
	    (int)b->size, (const char *)b->data);

err:	__wt_scr_free(&a);
	__wt_scr_free(&b);
	return (ret);
}
