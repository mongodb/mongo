/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __key_return --
 *     Change the cursor to reference an internal return key.
 */
static inline int
__key_return(WT_CURSOR_BTREE *cbt)
{
    WT_CURSOR *cursor;
    WT_ITEM *tmp;
    WT_PAGE *page;
    WT_ROW *rip;
    WT_SESSION_IMPL *session;

    page = cbt->ref->page;
    cursor = &cbt->iface;
    session = CUR2S(cbt);

    if (page->type == WT_PAGE_ROW_LEAF) {
        rip = &page->pg_row[cbt->slot];

        /*
         * If the cursor references a WT_INSERT item, take its key. Else, if we have an exact match,
         * we copied the key in the search function, take it from there. If we don't have an exact
         * match, take the key from the original page.
         */
        if (cbt->ins != NULL) {
            cursor->key.data = WT_INSERT_KEY(cbt->ins);
            cursor->key.size = WT_INSERT_KEY_SIZE(cbt->ins);
            return (0);
        }

        if (cbt->compare == 0) {
            /*
             * If not in an insert list and there's an exact match, the row-store search function
             * built the key we want to return in the cursor's temporary buffer. Swap the cursor's
             * search-key and temporary buffers so we can return it (it's unsafe to return the
             * temporary buffer itself because our caller might do another search in this table
             * using the key we return, and we'd corrupt the search key during any subsequent search
             * that used the temporary buffer).
             */
            tmp = cbt->row_key;
            cbt->row_key = cbt->tmp;
            cbt->tmp = tmp;

            cursor->key.data = cbt->row_key->data;
            cursor->key.size = cbt->row_key->size;
            return (0);
        }
        return (__wt_row_leaf_key(session, page, rip, &cursor->key, false));
    }

    /*
     * WT_PAGE_COL_FIX, WT_PAGE_COL_VAR:
     *	The interface cursor's record has usually been set, but that
     * isn't universally true, specifically, cursor.search_near may call
     * here without first setting the interface cursor.
     */
    cursor->recno = cbt->recno;
    return (0);
}

/*
 * __read_col_time_window --
 *     Retrieve the time window from a column store cell.
 */
static void
__read_col_time_window(WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL *cell, WT_TIME_WINDOW *tw)
{
    WT_CELL_UNPACK_KV unpack;

    __wt_cell_unpack_kv(session, page->dsk, cell, &unpack);
    WT_TIME_WINDOW_COPY(tw, &unpack.tw);
}

/*
 * __wt_read_row_time_window --
 *     Retrieve the time window from a row.
 */
void
__wt_read_row_time_window(WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip, WT_TIME_WINDOW *tw)
{
    WT_CELL_UNPACK_KV unpack;

    /*
     * Simple values are encoded at the time of reading a page into cache, in which case we set the
     * start time point as globally visible.
     */
    if (__wt_row_leaf_value_is_encoded(rip)) {
        WT_TIME_WINDOW_INIT(tw);
        return;
    }

    __wt_row_leaf_value_cell(session, page, rip, &unpack);
    WT_TIME_WINDOW_COPY(tw, &unpack.tw);
}

/*
 * __wt_col_fix_get_time_window --
 *     Look for a time window on a fixed-length column page.
 */
static bool
__wt_col_fix_get_time_window(
  WT_SESSION_IMPL *session, WT_REF *ref, uint64_t recno, WT_TIME_WINDOW *tw)
{
    WT_CELL *cell;
    WT_CELL_UNPACK_KV unpack;
    WT_PAGE *page;
    uint64_t start_recno, this_recno;
    u_int hi, lo, mid;

    page = ref->page;
    start_recno = ref->ref_recno;

    if (!WT_COL_FIX_TWS_SET(page))
        return (false);

    lo = 0;
    hi = page->pg_fix_numtws;
    /* There should always be at least one entry. */
    WT_ASSERT(session, lo < hi);

    /* Loop invariant: lo < hi. */
    for (;;) {
        /* If hi is lo+1, set mid to lo. Otherwise, hi is at least lo+2 and mid is between. */
        mid = (lo + hi) / 2;

        /* Check mid. */
        this_recno = start_recno + page->pg_fix_tws[mid].recno_offset;
        if (this_recno == recno) {
            cell = WT_COL_FIX_TW_CELL(page, &page->pg_fix_tws[mid]);
            __wt_cell_unpack_kv(session, page->dsk, cell, &unpack);
            WT_TIME_WINDOW_COPY(tw, &unpack.tw);
            return (true);
        }

        /* If we set mid to lo, we are done. */
        if (lo == mid)
            /* This was the last possible entry and we did not find it. */
            break;

        /* Otherwise, we either move lo up or hi down, but they cannot meet. */
        if (this_recno > recno)
            hi = mid;
        else
            lo = mid;

        WT_ASSERT(session, lo < hi);
    }
    return (false);
}

/*
 * __wt_read_cell_time_window --
 *     Read the time window from the cell.
 */
bool
__wt_read_cell_time_window(WT_CURSOR_BTREE *cbt, WT_TIME_WINDOW *tw)
{
    WT_PAGE *page;
    WT_SESSION_IMPL *session;

    session = CUR2S(cbt);
    page = cbt->ref->page;

    if (cbt->slot == UINT32_MAX)
        return (false);

    /* Take the value from the original page cell. */
    switch (page->type) {
    case WT_PAGE_ROW_LEAF:
        if (page->pg_row == NULL)
            return (false);
        __wt_read_row_time_window(session, page, &page->pg_row[cbt->slot], tw);
        break;
    case WT_PAGE_COL_VAR:
        if (page->pg_var == NULL)
            return (false);
        __read_col_time_window(session, page, WT_COL_PTR(page, &page->pg_var[cbt->slot]), tw);
        break;
    case WT_PAGE_COL_FIX:
        return (__wt_col_fix_get_time_window(session, cbt->ref, cbt->recno, tw));
    }
    return (true);
}

/*
 * __wt_value_return_buf --
 *     Change a buffer to reference an internal original-page return value.
 */
int
__wt_value_return_buf(WT_CURSOR_BTREE *cbt, WT_REF *ref, WT_ITEM *buf, WT_TIME_WINDOW *tw)
{
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK_KV unpack;
    WT_CURSOR *cursor;
    WT_PAGE *page;
    WT_ROW *rip;
    WT_SESSION_IMPL *session;
    uint8_t v;
    bool found;

    session = CUR2S(cbt);
    btree = S2BT(session);

    page = ref->page;
    cursor = &cbt->iface;

    if (page->type == WT_PAGE_ROW_LEAF) {
        rip = &page->pg_row[cbt->slot];

        /*
         * If a value is simple and is globally visible at the time of reading a page into cache, we
         * encode its location into the WT_ROW.
         */
        if (__wt_row_leaf_value(page, rip, buf)) {
            if (tw != NULL)
                WT_TIME_WINDOW_INIT(tw);
            return (0);
        }

        /* Take the value from the original page cell. */
        __wt_row_leaf_value_cell(session, page, rip, &unpack);
        if (tw != NULL)
            WT_TIME_WINDOW_COPY(tw, &unpack.tw);
        return (__wt_page_cell_data_ref(session, page, &unpack, buf));
    }

    if (page->type == WT_PAGE_COL_VAR) {
        /* Take the value from the original page cell. */
        cell = WT_COL_PTR(page, &page->pg_var[cbt->slot]);
        __wt_cell_unpack_kv(session, page->dsk, cell, &unpack);
        if (tw != NULL)
            WT_TIME_WINDOW_COPY(tw, &unpack.tw);
        return (__wt_page_cell_data_ref(session, page, &unpack, buf));
    }

    /*
     * WT_PAGE_COL_FIX: Take the value from the original page.
     */
    if (tw != NULL) {
        found = __wt_col_fix_get_time_window(session, ref, cbt->recno, tw);
        if (!found)
            WT_TIME_WINDOW_INIT(tw);
    }
    v = __bit_getv_recno(ref, cursor->recno, btree->bitcnt);
    return (__wt_buf_set(session, buf, &v, 1));
}

/*
 * __wt_key_return --
 *     Change the cursor to reference an internal return key.
 */
int
__wt_key_return(WT_CURSOR_BTREE *cbt)
{
    WT_CURSOR *cursor;

    cursor = &cbt->iface;

    /*
     * We may already have an internal key and the cursor may not be set up to get another copy, so
     * we have to leave it alone. Consider a cursor search followed by an update: the update doesn't
     * repeat the search, it simply updates the currently referenced key's value. We will end up
     * here with the correct internal key, but we can't "return" the key again even if we wanted to
     * do the additional work, the cursor isn't set up for that because we didn't just complete a
     * search.
     */
    F_CLR(cursor, WT_CURSTD_KEY_EXT);
    if (!F_ISSET(cursor, WT_CURSTD_KEY_INT)) {
        WT_RET(__key_return(cbt));
        F_SET(cursor, WT_CURSTD_KEY_INT);
    }
    return (0);
}

/*
 * __wt_value_return --
 *     Change the cursor to reference an update return value.
 */
void
__wt_value_return(WT_CURSOR_BTREE *cbt, WT_UPDATE_VALUE *upd_value)
{
    WT_CURSOR *cursor;

    cursor = &cbt->iface;

    F_CLR(cursor, WT_CURSTD_VALUE_EXT);
    /*
     * We're passed a "standard" update that's visible to us. Our caller should have already checked
     * for deleted items (we're too far down the call stack to return not-found) and any modify
     * updates should have been reconstructed into a full standard update.
     *
     * We are here to return a value to the caller. Make sure we don't skip the buf.
     */
    WT_ASSERT(CUR2S(cbt), upd_value->type == WT_UPDATE_STANDARD && !upd_value->skip_buf);
    cursor->value.data = upd_value->buf.data;
    cursor->value.size = upd_value->buf.size;

    F_SET(cursor, WT_CURSTD_VALUE_INT);
}
