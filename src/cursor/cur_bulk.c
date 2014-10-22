/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __curbulk_insert_fix --
 *	Fixed-length column-store bulk cursor insert.
 */
static int
__curbulk_insert_fix(WT_CURSOR *cursor)
{
	WT_BTREE *btree;
	WT_CURSOR_BULK *cbulk;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbulk = (WT_CURSOR_BULK *)cursor;
	btree = cbulk->cbt.btree;

	/*
	 * Bulk cursor inserts are updates, but don't need auto-commit
	 * transactions because they are single-threaded and not visible
	 * until the bulk cursor is closed.
	 */
	CURSOR_API_CALL(cursor, session, insert, btree);

	WT_CURSOR_NEEDVALUE(cursor);

	WT_ERR(__wt_bulk_insert_fix(session, cbulk));

	WT_STAT_FAST_DATA_INCR(session, cursor_insert_bulk);

err:	API_END_RET(session, ret);
}

/*
 * __curbulk_insert_var --
 *	Variable-length column-store bulk cursor insert.
 */
static int
__curbulk_insert_var(WT_CURSOR *cursor)
{
	WT_BTREE *btree;
	WT_CURSOR_BULK *cbulk;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	int duplicate;

	cbulk = (WT_CURSOR_BULK *)cursor;
	btree = cbulk->cbt.btree;

	/*
	 * Bulk cursor inserts are updates, but don't need auto-commit
	 * transactions because they are single-threaded and not visible
	 * until the bulk cursor is closed.
	 */
	CURSOR_API_CALL(cursor, session, insert, btree);

	WT_CURSOR_NEEDVALUE(cursor);

	/*
	 * If this isn't the first value inserted, compare it against the last
	 * value and increment the RLE count.
	 *
	 * Instead of a "first time" variable, I'm using the RLE count, because
	 * it is only zero before the first row is inserted.
	 */
	duplicate = 0;
	if (cbulk->rle != 0) {
		if (cbulk->last.size == cursor->value.size &&
		    memcmp(cbulk->last.data, cursor->value.data,
		    cursor->value.size) == 0) {
			++cbulk->rle;
			duplicate = 1;
		} else
			WT_ERR(__wt_bulk_insert_var(session, cbulk));
	}

	/*
	 * Save a copy of the value for the next comparison and reset the RLE
	 * counter.
	 */
	if (!duplicate) {
		WT_ERR(__wt_buf_set(session,
		    &cbulk->last, cursor->value.data, cursor->value.size));
		cbulk->rle = 1;
	}

	WT_STAT_FAST_DATA_INCR(session, cursor_insert_bulk);

err:	API_END_RET(session, ret);
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
	    session, b, cbulk->last.data, cbulk->last.size));

	WT_ERR_MSG(session, EINVAL,
	    "bulk-load presented with out-of-order keys: %.*s compares smaller "
	    "than previously inserted key %.*s",
	    (int)a->size, (const char *)a->data,
	    (int)b->size, (const char *)b->data);

err:	__wt_scr_free(&a);
	__wt_scr_free(&b);
	return (ret);
}

/*
 * __curbulk_insert_row --
 *	Row-store bulk cursor insert, with key-sort checks.
 */
static int
__curbulk_insert_row(WT_CURSOR *cursor)
{
	WT_BTREE *btree;
	WT_CURSOR_BULK *cbulk;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	int cmp;

	cbulk = (WT_CURSOR_BULK *)cursor;
	btree = cbulk->cbt.btree;

	/*
	 * Bulk cursor inserts are updates, but don't need auto-commit
	 * transactions because they are single-threaded and not visible
	 * until the bulk cursor is closed.
	 */
	CURSOR_API_CALL(cursor, session, insert, btree);

	WT_CURSOR_CHECKKEY(cursor);
	WT_CURSOR_CHECKVALUE(cursor);

	/*
	 * If this isn't the first key inserted, compare it against the last key
	 * to ensure the application doesn't accidentally corrupt the table.
	 *
	 * Instead of a "first time" variable, I'm using the RLE count, because
	 * it is only zero before the first row is inserted.
	 */
	if (cbulk->rle != 0) {
		WT_ERR(__wt_compare(session,
		    btree->collator, &cursor->key, &cbulk->last, &cmp));
		if (cmp <= 0)
			WT_ERR(__bulk_row_keycmp_err(cbulk));
	}

	/*
	 * Save a copy of the key for the next comparison and set the RLE
	 * counter.
	 */
	WT_ERR(__wt_buf_set(session,
	    &cbulk->last, cursor->key.data, cursor->key.size));
	cbulk->rle = 1;

	WT_ERR(__wt_bulk_insert_row(session, cbulk));

	WT_STAT_FAST_DATA_INCR(session, cursor_insert_bulk);

err:	API_END_RET(session, ret);
}

/*
 * __curbulk_insert_row_skip_check --
 *	Row-store bulk cursor insert, without key-sort checks.
 */
static int
__curbulk_insert_row_skip_check(WT_CURSOR *cursor)
{
	WT_BTREE *btree;
	WT_CURSOR_BULK *cbulk;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbulk = (WT_CURSOR_BULK *)cursor;
	btree = cbulk->cbt.btree;

	/*
	 * Bulk cursor inserts are updates, but don't need auto-commit
	 * transactions because they are single-threaded and not visible
	 * until the bulk cursor is closed.
	 */
	CURSOR_API_CALL(cursor, session, insert, btree);

	WT_CURSOR_NEEDKEY(cursor);
	WT_CURSOR_NEEDVALUE(cursor);

	WT_ERR(__wt_bulk_insert_row(session, cbulk));

	WT_STAT_FAST_DATA_INCR(session, cursor_insert_bulk);

err:	API_END_RET(session, ret);
}

/*
 * __curbulk_close --
 *	WT_CURSOR->close for the bulk cursor type.
 */
static int
__curbulk_close(WT_CURSOR *cursor)
{
	WT_BTREE *btree;
	WT_CURSOR_BULK *cbulk;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbulk = (WT_CURSOR_BULK *)cursor;
	btree = cbulk->cbt.btree;

	CURSOR_API_CALL(cursor, session, close, btree);

	WT_TRET(__wt_bulk_wrapup(session, cbulk));
	__wt_buf_free(session, &cbulk->last);

	WT_TRET(__wt_session_release_btree(session));

	/* The URI is owned by the btree handle. */
	cursor->internal_uri = NULL;

	WT_TRET(__wt_cursor_close(cursor));

err:	API_END_RET(session, ret);
}

/*
 * __wt_curbulk_init --
 *	Initialize a bulk cursor.
 */
int
__wt_curbulk_init(WT_SESSION_IMPL *session,
    WT_CURSOR_BULK *cbulk, int bitmap, int skip_sort_check)
{
	WT_CURSOR *c;
	WT_CURSOR_BTREE *cbt;

	c = &cbulk->cbt.iface;
	cbt = &cbulk->cbt;

	/* Bulk cursors only support insert and close (reset is a no-op). */
	__wt_cursor_set_notsup(c);
	switch (cbt->btree->type) {
	case BTREE_COL_FIX:
		c->insert = __curbulk_insert_fix;
		break;
	case BTREE_COL_VAR:
		c->insert = __curbulk_insert_var;
		break;
	case BTREE_ROW:
		c->insert = skip_sort_check ?
		    __curbulk_insert_row_skip_check : __curbulk_insert_row;
		break;
	WT_ILLEGAL_VALUE(session);
	}
	c->close = __curbulk_close;

	cbulk->bitmap = bitmap;
	if (bitmap)
		F_SET(c, WT_CURSTD_RAW);

	return (__wt_bulk_init(session, cbulk));
}
