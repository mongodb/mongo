/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __bulk_col_keycmp_err --
 *     Error routine when column-store keys inserted out-of-order.
 */
static int
__bulk_col_keycmp_err(WT_CURSOR_BULK *cbulk)
{
    WT_CURSOR *cursor;
    WT_SESSION_IMPL *session;

    cursor = &cbulk->cbt.iface;
    session = CUR2S(cbulk);

    WT_RET_MSG(session, EINVAL,
      "bulk-load presented with out-of-order keys: %" PRIu64
      " is less than previously inserted key %" PRIu64,
      cursor->recno, cbulk->recno);
}

/*
 * __curbulk_insert_fix --
 *     Fixed-length column-store bulk cursor insert.
 */
static int
__curbulk_insert_fix(WT_CURSOR *cursor)
{
    WT_BTREE *btree;
    WT_CURSOR_BULK *cbulk;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    uint64_t recno;

    cbulk = (WT_CURSOR_BULK *)cursor;
    btree = CUR2BT(&cbulk->cbt);

    /*
     * Bulk cursor inserts are updates, but don't need auto-commit transactions because they are
     * single-threaded and not visible until the bulk cursor is closed.
     */
    CURSOR_API_CALL(cursor, session, insert, btree);
    WT_STAT_CONN_DATA_INCR(session, cursor_insert_bulk);

    /*
     * If the "append" flag was configured, the application doesn't have to supply a key, else
     * require a key.
     */
    if (F_ISSET(cursor, WT_CURSTD_APPEND))
        recno = cbulk->recno + 1;
    else {
        WT_ERR(__cursor_checkkey(cursor));
        if ((recno = cursor->recno) <= cbulk->recno)
            WT_ERR(__bulk_col_keycmp_err(cbulk));
    }
    WT_ERR(__cursor_checkvalue(cursor));

    /*
     * Insert any skipped records as deleted records, update the current record count.
     */
    for (; recno != cbulk->recno + 1; ++cbulk->recno)
        WT_ERR(__wt_bulk_insert_fix(session, cbulk, true));
    cbulk->recno = recno;

    /* Insert the current record. */
    ret = __wt_bulk_insert_fix(session, cbulk, false);

err:
    API_END_RET(session, ret);
}

/*
 * __curbulk_insert_fix_bitmap --
 *     Fixed-length column-store bulk cursor insert for bitmaps.
 */
static int
__curbulk_insert_fix_bitmap(WT_CURSOR *cursor)
{
    WT_BTREE *btree;
    WT_CURSOR_BULK *cbulk;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cbulk = (WT_CURSOR_BULK *)cursor;
    btree = CUR2BT(&cbulk->cbt);

    /*
     * Bulk cursor inserts are updates, but don't need auto-commit transactions because they are
     * single-threaded and not visible until the bulk cursor is closed.
     */
    CURSOR_API_CALL(cursor, session, insert, btree);
    WT_STAT_CONN_DATA_INCR(session, cursor_insert_bulk);

    WT_ERR(__cursor_checkvalue(cursor));

    /* Insert the current record. */
    ret = __wt_bulk_insert_fix_bitmap(session, cbulk);

err:
    API_END_RET(session, ret);
}

/*
 * __curbulk_insert_var --
 *     Variable-length column-store bulk cursor insert.
 */
static int
__curbulk_insert_var(WT_CURSOR *cursor)
{
    WT_BTREE *btree;
    WT_CURSOR_BULK *cbulk;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    uint64_t recno;

    cbulk = (WT_CURSOR_BULK *)cursor;
    btree = CUR2BT(&cbulk->cbt);

    /*
     * Bulk cursor inserts are updates, but don't need auto-commit transactions because they are
     * single-threaded and not visible until the bulk cursor is closed.
     */
    CURSOR_API_CALL(cursor, session, insert, btree);
    WT_STAT_CONN_DATA_INCR(session, cursor_insert_bulk);

    /*
     * If the "append" flag was configured, the application doesn't have to supply a key, else
     * require a key.
     */
    if (F_ISSET(cursor, WT_CURSTD_APPEND))
        recno = cbulk->recno + 1;
    else {
        WT_ERR(__cursor_checkkey(cursor));
        if ((recno = cursor->recno) <= cbulk->recno)
            WT_ERR(__bulk_col_keycmp_err(cbulk));
    }
    WT_ERR(__cursor_checkvalue(cursor));

    if (!cbulk->first_insert) {
        /*
         * If not the first insert and the key space is sequential, compare the current value
         * against the last value; if the same, just increment the RLE count.
         */
        if (recno == cbulk->recno + 1 && cbulk->last->size == cursor->value.size &&
          memcmp(cbulk->last->data, cursor->value.data, cursor->value.size) == 0) {
            ++cbulk->rle;
            ++cbulk->recno;
            goto duplicate;
        }

        /* Insert the previous key/value pair. */
        WT_ERR(__wt_bulk_insert_var(session, cbulk, false));
    } else
        cbulk->first_insert = false;

    /*
     * Insert any skipped records as deleted records, update the current record count and RLE
     * counter.
     */
    if (recno != cbulk->recno + 1) {
        cbulk->rle = (recno - cbulk->recno) - 1;
        WT_ERR(__wt_bulk_insert_var(session, cbulk, true));
    }
    cbulk->rle = 1;
    cbulk->recno = recno;

    /* Save a copy of the value for the next comparison. */
    ret = __wt_buf_set(session, cbulk->last, cursor->value.data, cursor->value.size);

duplicate:
err:
    API_END_RET(session, ret);
}

/*
 * __bulk_row_keycmp_err --
 *     Error routine when row-store keys inserted out-of-order.
 */
static int
__bulk_row_keycmp_err(WT_CURSOR_BULK *cbulk)
{
    WT_CURSOR *cursor;
    WT_DECL_ITEM(a);
    WT_DECL_ITEM(b);
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cursor = &cbulk->cbt.iface;
    session = CUR2S(cbulk);

    WT_ERR(__wt_scr_alloc(session, 512, &a));
    WT_ERR(__wt_scr_alloc(session, 512, &b));

    WT_ERR_MSG(session, EINVAL,
      "bulk-load presented with out-of-order keys: %s compares smaller than previously inserted "
      "key %s",
      __wt_buf_set_printable(session, cursor->key.data, cursor->key.size, false, a),
      __wt_buf_set_printable(session, cbulk->last->data, cbulk->last->size, false, b));

err:
    __wt_scr_free(session, &a);
    __wt_scr_free(session, &b);
    return (ret);
}

/*
 * __curbulk_insert_row --
 *     Row-store bulk cursor insert, with key-sort checks.
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
    btree = CUR2BT(&cbulk->cbt);

    /*
     * Bulk cursor inserts are updates, but don't need auto-commit transactions because they are
     * single-threaded and not visible until the bulk cursor is closed.
     */
    CURSOR_API_CALL(cursor, session, insert, btree);
    WT_STAT_CONN_DATA_INCR(session, cursor_insert_bulk);

    WT_ERR(__cursor_checkkey(cursor));
    WT_ERR(__cursor_checkvalue(cursor));

    /*
     * If this isn't the first key inserted, compare it against the last key to ensure the
     * application doesn't accidentally corrupt the table.
     */
    if (!cbulk->first_insert) {
        WT_ERR(__wt_compare(session, btree->collator, &cursor->key, cbulk->last, &cmp));
        if (cmp <= 0)
            WT_ERR(__bulk_row_keycmp_err(cbulk));
    } else
        cbulk->first_insert = false;

    /* Save a copy of the key for the next comparison. */
    WT_ERR(__wt_buf_set(session, cbulk->last, cursor->key.data, cursor->key.size));

    ret = __wt_bulk_insert_row(session, cbulk);

err:
    API_END_RET(session, ret);
}

/*
 * __curbulk_insert_row_skip_check --
 *     Row-store bulk cursor insert, without key-sort checks.
 */
static int
__curbulk_insert_row_skip_check(WT_CURSOR *cursor)
{
    WT_BTREE *btree;
    WT_CURSOR_BULK *cbulk;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cbulk = (WT_CURSOR_BULK *)cursor;
    btree = CUR2BT(&cbulk->cbt);

    /*
     * Bulk cursor inserts are updates, but don't need auto-commit transactions because they are
     * single-threaded and not visible until the bulk cursor is closed.
     */
    CURSOR_API_CALL(cursor, session, insert, btree);
    WT_STAT_CONN_DATA_INCR(session, cursor_insert_bulk);

    WT_ERR(__cursor_checkkey(cursor));
    WT_ERR(__cursor_checkvalue(cursor));

    ret = __wt_bulk_insert_row(session, cbulk);

err:
    API_END_RET(session, ret);
}

/*
 * __wt_curbulk_init --
 *     Initialize a bulk cursor.
 */
int
__wt_curbulk_init(
  WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk, bool bitmap, bool skip_sort_check)
{
    WT_CURSOR *cursor;
    WT_CURSOR_BTREE *cbt;

    cursor = &cbulk->cbt.iface;
    cbt = &cbulk->cbt;

    /* Bulk cursors only support insert and close (reset is a no-op). */
    __wt_cursor_set_notsup(cursor);
    switch (CUR2BT(cbt)->type) {
    case BTREE_COL_FIX:
        cursor->insert = bitmap ? __curbulk_insert_fix_bitmap : __curbulk_insert_fix;
        break;
    case BTREE_COL_VAR:
        cursor->insert = __curbulk_insert_var;
        break;
    case BTREE_ROW:
        /*
         * Row-store order comparisons are expensive, so we optionally skip them when we know the
         * input is correct.
         */
        cursor->insert = skip_sort_check ? __curbulk_insert_row_skip_check : __curbulk_insert_row;
        break;
    }

    cbulk->first_insert = true;
    cbulk->recno = 0;
    cbulk->bitmap = bitmap;
    if (bitmap)
        F_SET(cursor, WT_CURSTD_RAW);

    /*
     * The bulk last buffer is used to detect out-of-order keys in row-store to avoid corruption,
     * and to detect duplicate values in variable-length column-store, where we increment the RLE
     * instead of storing another value. In variable-length column-store, if the first two values we
     * load are zero-length, the first one will set the last buffer's data field to NULL, and the
     * second will cause us to call the underlying memory comparison function with a NULL pointer,
     * which triggers run-time analyzers. Give the buffer some memory to avoid the problem (h/t to
     * C99 typos).
     */
    WT_RET(__wt_scr_alloc(session, 100, &cbulk->last));

    return (__wt_bulk_init(session, cbulk));
}

/*
 * __wt_curbulk_close --
 *     Close a bulk cursor.
 */
int
__wt_curbulk_close(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk)
{
    WT_DECL_RET;

    ret = __wt_bulk_wrapup(session, cbulk);

    __wt_scr_free(session, &cbulk->last);
    return (ret);
}
