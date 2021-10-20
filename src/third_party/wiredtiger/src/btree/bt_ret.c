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
    session = (WT_SESSION_IMPL *)cbt->iface.session;

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
 * __value_return --
 *     Change the cursor to reference an internal original-page return value.
 */
static inline int
__value_return(WT_CURSOR_BTREE *cbt)
{
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK unpack;
    WT_CURSOR *cursor;
    WT_PAGE *page;
    WT_ROW *rip;
    WT_SESSION_IMPL *session;
    uint8_t v;

    session = (WT_SESSION_IMPL *)cbt->iface.session;
    btree = S2BT(session);

    page = cbt->ref->page;
    cursor = &cbt->iface;

    if (page->type == WT_PAGE_ROW_LEAF) {
        rip = &page->pg_row[cbt->slot];

        /* Simple values have their location encoded in the WT_ROW. */
        if (__wt_row_leaf_value(page, rip, &cursor->value))
            return (0);

        /* Take the value from the original page cell. */
        __wt_row_leaf_value_cell(page, rip, NULL, &unpack);
        return (__wt_page_cell_data_ref(session, page, &unpack, &cursor->value));
    }

    if (page->type == WT_PAGE_COL_VAR) {
        /* Take the value from the original page cell. */
        cell = WT_COL_PTR(page, &page->pg_var[cbt->slot]);
        __wt_cell_unpack(cell, &unpack);
        return (__wt_page_cell_data_ref(session, page, &unpack, &cursor->value));
    }

    /* WT_PAGE_COL_FIX: Take the value from the original page. */
    v = __bit_getv_recno(cbt->ref, cursor->recno, btree->bitcnt);
    return (__wt_buf_set(session, &cursor->value, &v, 1));
}

/*
 * When threads race modifying a record, we can end up with more than the usual maximum number of
 * modifications in an update list. We'd prefer not to allocate memory in a return path, so add a
 * few additional slots to the array we use to build up a list of modify records to apply.
 */
#define WT_MODIFY_ARRAY_SIZE (WT_MAX_MODIFY_UPDATE + 10)

/*
 * __wt_value_return_upd --
 *     Change the cursor to reference an internal update structure return value.
 */
int
__wt_value_return_upd(WT_CURSOR_BTREE *cbt, WT_UPDATE *upd, bool ignore_visibility)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    WT_UPDATE **listp, *list[WT_MODIFY_ARRAY_SIZE];
    size_t allocated_bytes;
    u_int i;
    bool skipped_birthmark;

    cursor = &cbt->iface;
    session = (WT_SESSION_IMPL *)cbt->iface.session;
    allocated_bytes = 0;

    /*
     * We're passed a "standard" or "modified"  update that's visible to us.
     * Our caller should have already checked for deleted items (we're too
     * far down the call stack to return not-found).
     *
     * Fast path if it's a standard item, assert our caller's behavior.
     */
    if (upd->type == WT_UPDATE_STANDARD) {
        cursor->value.data = upd->data;
        cursor->value.size = upd->size;
        return (0);
    }
    WT_ASSERT(session, upd->type == WT_UPDATE_MODIFY);

    /*
     * Find a complete update that's visible to us, tracking modifications that are visible to us.
     */
    for (i = 0, listp = list, skipped_birthmark = false; upd != NULL; upd = upd->next) {
        if (upd->txnid == WT_TXN_ABORTED)
            continue;

        if (!ignore_visibility && !__wt_txn_upd_visible(session, upd)) {
            if (upd->type == WT_UPDATE_BIRTHMARK)
                skipped_birthmark = true;
            continue;
        }

        if (upd->type == WT_UPDATE_BIRTHMARK) {
            upd = NULL;
            break;
        }

        if (WT_UPDATE_DATA_VALUE(upd))
            break;

        if (upd->type == WT_UPDATE_MODIFY) {
            /*
             * Update lists are expected to be short, but it's not guaranteed. There's sufficient
             * room on the stack to avoid memory allocation in normal cases, but we have to handle
             * the edge cases too.
             */
            if (i >= WT_MODIFY_ARRAY_SIZE) {
                if (i == WT_MODIFY_ARRAY_SIZE)
                    listp = NULL;
                WT_ERR(__wt_realloc_def(session, &allocated_bytes, i + 1, &listp));
                if (i == WT_MODIFY_ARRAY_SIZE)
                    memcpy(listp, list, sizeof(list));
            }
            listp[i++] = upd;

            /*
             * Once a modify is found, all previously committed modifications should be applied
             * regardless of visibility.
             */
            ignore_visibility = true;
        }
    }

    /*
     * If there's no visible update and we skipped a birthmark, the base item is an empty item (in
     * other words, birthmarks we can't read act as tombstones). If there's no visible update and we
     * didn't skip a birthmark, the base item is the on-page item, which must be globally visible.
     * If there's a visible update and it's a tombstone, the base item is an empty item. If there's
     * a visible update and it's not a tombstone, the base item is the on-page item.
     */
    if (upd == NULL) {
        if (skipped_birthmark)
            WT_ERR(__wt_buf_set(session, &cursor->value, "", 0));
        else {
            /*
             * Callers of this function set the cursor slot to an impossible value to check we don't
             * try and return on-page values when the update list should have been sufficient (which
             * happens, for example, if an update list was truncated, deleting some standard update
             * required by a previous modify update). Assert the case.
             */
            WT_ASSERT(session, cbt->slot != UINT32_MAX);

            WT_ERR(__value_return(cbt));
        }
    } else if (upd->type == WT_UPDATE_TOMBSTONE)
        WT_ERR(__wt_buf_set(session, &cursor->value, "", 0));
    else
        WT_ERR(__wt_buf_set(session, &cursor->value, upd->data, upd->size));

    /*
     * Once we have a base item, roll forward through any visible modify updates.
     */
    while (i > 0)
        WT_ERR(__wt_modify_apply(session, cursor, listp[--i]->data));

err:
    if (allocated_bytes != 0)
        __wt_free(session, listp);
    return (ret);
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
 *     Change the cursor to reference an internal return value.
 */
int
__wt_value_return(WT_CURSOR_BTREE *cbt, WT_UPDATE *upd)
{
    WT_CURSOR *cursor;

    cursor = &cbt->iface;

    F_CLR(cursor, WT_CURSTD_VALUE_EXT);
    if (upd == NULL)
        WT_RET(__value_return(cbt));
    else
        WT_RET(__wt_value_return_upd(cbt, upd, false));
    F_SET(cursor, WT_CURSTD_VALUE_INT);
    return (0);
}
