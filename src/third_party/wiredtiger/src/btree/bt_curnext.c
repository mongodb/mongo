/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __cursor_fix_append_next --
 *     Return the next entry on the append list.
 */
static inline int
__cursor_fix_append_next(WT_CURSOR_BTREE *cbt, bool newpage, bool restart)
{
    WT_SESSION_IMPL *session;

    session = CUR2S(cbt);

    /* If restarting after a prepare conflict, jump to the right spot. */
    if (restart)
        goto restart_read;

    if (newpage) {
        if ((cbt->ins = WT_SKIP_FIRST(cbt->ins_head)) == NULL)
            return (WT_NOTFOUND);
    } else if (cbt->recno >= WT_INSERT_RECNO(cbt->ins) &&
      (cbt->ins = WT_SKIP_NEXT(cbt->ins)) == NULL)
        return (WT_NOTFOUND);

    /*
     * This code looks different from the cursor-previous code. The append list may be preceded by
     * other rows, which means the cursor's recno will be set to a value and we simply want to
     * increment it. If the cursor's recno is NOT set, we're starting an iteration in a tree with
     * only appended items. In that case, recno will be 0 and happily enough the increment will set
     * it to 1, which is correct.
     */
    __cursor_set_recno(cbt, cbt->recno + 1);

    /*
     * Fixed-width column store appends are inherently non-transactional. Even a non-visible update
     * by a concurrent or aborted transaction changes the effective end of the data. The effect is
     * subtle because of the blurring between deleted and empty values, but ideally we would skip
     * all uncommitted changes at the end of the data. This doesn't apply to variable-width column
     * stores because the implicitly created records written by reconciliation are deleted and so
     * can be never seen by a read.
     *
     * The problem is that we don't know at this point whether there may be multiple uncommitted
     * changes at the end of the data, and it would be expensive to check every time we hit an
     * aborted update. If an insert is aborted, we simply return zero (empty), regardless of whether
     * we are at the end of the data.
     */
    if (cbt->recno < WT_INSERT_RECNO(cbt->ins)) {
        cbt->v = 0;
        cbt->iface.value.data = &cbt->v;
        cbt->iface.value.size = 1;
    } else {
restart_read:
        WT_RET(__wt_txn_read_upd_list(session, cbt, cbt->ins->upd));
        if (cbt->upd_value->type == WT_UPDATE_INVALID ||
          cbt->upd_value->type == WT_UPDATE_TOMBSTONE) {
            cbt->v = 0;
            cbt->iface.value.data = &cbt->v;
            cbt->iface.value.size = 1;
        } else
            __wt_value_return(cbt, cbt->upd_value);
    }
    return (0);
}

/*
 * __cursor_fix_next --
 *     Move to the next, fixed-length column-store item.
 */
static inline int
__cursor_fix_next(WT_CURSOR_BTREE *cbt, bool newpage, bool restart)
{
    WT_PAGE *page;
    WT_SESSION_IMPL *session;

    session = CUR2S(cbt);
    page = cbt->ref->page;

    /* If restarting after a prepare conflict, jump to the right spot. */
    if (restart)
        goto restart_read;

    /* Initialize for each new page. */
    if (newpage) {
        /*
         * Be paranoid and set the slot out of bounds when moving to a new page.
         */
        cbt->slot = UINT32_MAX;
        cbt->last_standard_recno = __col_fix_last_recno(cbt->ref);
        if (cbt->last_standard_recno == 0)
            return (WT_NOTFOUND);
        __cursor_set_recno(cbt, cbt->ref->ref_recno);
        goto new_page;
    }

    /* Move to the next entry and return the item. */
    if (cbt->recno >= cbt->last_standard_recno)
        return (WT_NOTFOUND);
    __cursor_set_recno(cbt, cbt->recno + 1);

new_page:
restart_read:
    /* We only have one slot. */
    cbt->slot = 0;

    /* Check any insert list for a matching record. */
    cbt->ins_head = WT_COL_UPDATE_SINGLE(page);
    cbt->ins = __col_insert_search(cbt->ins_head, cbt->ins_stack, cbt->next_stack, cbt->recno);
    if (cbt->ins != NULL && cbt->recno != WT_INSERT_RECNO(cbt->ins))
        cbt->ins = NULL;
    __wt_upd_value_clear(cbt->upd_value);
    if (cbt->ins != NULL)
        /* Check the update list. */
        WT_RET(__wt_txn_read_upd_list(session, cbt, cbt->ins->upd));
    if (cbt->upd_value->type == WT_UPDATE_INVALID)
        /*
         * Read the on-disk value and/or history. Pass an update list: the update list may contain
         * the base update for a modify chain after rollback-to-stable, required for correctness.
         */
        WT_RET(__wt_txn_read(session, cbt, NULL, cbt->recno, cbt->ins ? cbt->ins->upd : NULL));
    if (cbt->upd_value->type == WT_UPDATE_TOMBSTONE || cbt->upd_value->type == WT_UPDATE_INVALID) {
        /*
         * Deleted values read as 0.
         *
         * Getting an invalid update back means that there was no update, the on-disk value isn't
         * visible, and there isn't anything in history either. This means this chunk of the tree
         * didn't exist yet for us (given our read timestamp), so we can either return NOTFOUND or
         * produce a zero value depending on the desired end-of-tree semantics. For now, we produce
         * zero so as not to change the preexisting end-of-tree behavior.
         */
        cbt->v = 0;
        cbt->iface.value.data = &cbt->v;
        cbt->iface.value.size = 1;
    } else
        __wt_value_return(cbt, cbt->upd_value);

    return (0);
}

/*
 * __cursor_var_append_next --
 *     Return the next variable-length entry on the append list.
 */
static inline int
__cursor_var_append_next(
  WT_CURSOR_BTREE *cbt, bool newpage, bool restart, size_t *skippedp, bool *key_out_of_boundsp)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = CUR2S(cbt);
    *skippedp = 0;

    /* If restarting after a prepare conflict, jump to the right spot. */
    if (restart)
        goto restart_read;

    if (newpage) {
        cbt->ins = WT_SKIP_FIRST(cbt->ins_head);
        goto new_page;
    }

    for (;;) {
        cbt->ins = WT_SKIP_NEXT(cbt->ins);
new_page:
        if (cbt->ins == NULL)
            return (WT_NOTFOUND);
        __cursor_set_recno(cbt, WT_INSERT_RECNO(cbt->ins));

restart_read:
        /*
         * If an upper bound has been set ensure that the key is within the range, otherwise early
         * exit.
         */
        if ((ret = __wt_btcur_bounds_early_exit(session, cbt, true, key_out_of_boundsp)) ==
          WT_NOTFOUND)
            WT_STAT_CONN_DATA_INCR(session, cursor_bounds_next_early_exit);
        WT_RET(ret);

        WT_RET(__wt_txn_read_upd_list(session, cbt, cbt->ins->upd));

        if (cbt->upd_value->type == WT_UPDATE_INVALID) {
            ++*skippedp;
            continue;
        }
        if (cbt->upd_value->type == WT_UPDATE_TOMBSTONE) {
            if (cbt->upd_value->tw.stop_txn != WT_TXN_NONE &&
              __wt_txn_upd_value_visible_all(session, cbt->upd_value))
                ++cbt->page_deleted_count;
            ++*skippedp;
            continue;
        }
        __wt_value_return(cbt, cbt->upd_value);
        return (0);
    }
    /* NOTREACHED */
}

/*
 * __cursor_var_next --
 *     Move to the next, variable-length column-store item.
 */
static inline int
__cursor_var_next(
  WT_CURSOR_BTREE *cbt, bool newpage, bool restart, size_t *skippedp, bool *key_out_of_boundsp)
{
    WT_CELL *cell;
    WT_CELL_UNPACK_KV unpack;
    WT_COL *cip;
    WT_DECL_RET;
    WT_INSERT *ins;
    WT_PAGE *page;
    WT_SESSION_IMPL *session;
    uint64_t rle, rle_start;

    session = CUR2S(cbt);
    page = cbt->ref->page;

    rle_start = 0; /* -Werror=maybe-uninitialized */
    *skippedp = 0;

    /* If restarting after a prepare conflict, jump to the right spot. */
    if (restart)
        goto restart_read;

    /* Initialize for each new page. */
    if (newpage) {
        /*
         * Be paranoid and set the slot out of bounds when moving to a new page.
         */
        cbt->slot = UINT32_MAX;
        cbt->last_standard_recno = __col_var_last_recno(cbt->ref);
        if (cbt->last_standard_recno == 0)
            return (WT_NOTFOUND);
        __cursor_set_recno(cbt, cbt->ref->ref_recno);
        cbt->cip_saved = NULL;
        F_CLR(cbt, WT_CBT_CACHEABLE_RLE_CELL);
        goto new_page;
    }

    /* Move to the next entry and return the item. */
    for (;;) {
        if (cbt->recno >= cbt->last_standard_recno)
            return (WT_NOTFOUND);
        __cursor_set_recno(cbt, cbt->recno + 1);

new_page:
restart_read:
        /*
         * If an upper bound has been set ensure that the key is within the range, otherwise early
         * exit. In the case where there is a large set of RLE deleted records it is possible that
         * calculated recno will be off the end of the page. We don't need to add an additional
         * check for this case as the next iteration, either on a page or append list will check the
         * recno and early exit. It does present a potential optimization but to keep the bounded
         * cursor logic simple we will forego it for now.
         */
        if ((ret = __wt_btcur_bounds_early_exit(session, cbt, true, key_out_of_boundsp)) ==
          WT_NOTFOUND)
            WT_STAT_CONN_DATA_INCR(session, cursor_bounds_next_early_exit);
        WT_RET(ret);

        /* Find the matching WT_COL slot. */
        if ((cip = __col_var_search(cbt->ref, cbt->recno, &rle_start)) == NULL)
            return (WT_NOTFOUND);
        cbt->slot = WT_COL_SLOT(page, cip);

        /* Check any insert list for a matching record. */
        cbt->ins_head = WT_COL_UPDATE_SLOT(page, cbt->slot);
        cbt->ins = __col_insert_search_match(cbt->ins_head, cbt->recno);
        __wt_upd_value_clear(cbt->upd_value);
        if (cbt->ins != NULL)
            WT_RET(__wt_txn_read_upd_list(session, cbt, cbt->ins->upd));
        if (cbt->upd_value->type != WT_UPDATE_INVALID) {
            if (cbt->upd_value->type == WT_UPDATE_TOMBSTONE) {
                if (cbt->upd_value->tw.stop_txn != WT_TXN_NONE &&
                  __wt_txn_upd_value_visible_all(session, cbt->upd_value))
                    ++cbt->page_deleted_count;
                ++*skippedp;
                continue;
            }
            __wt_value_return(cbt, cbt->upd_value);
            return (0);
        }

        /*
         * There's no matching insert list item. If we're on the same slot as the last reference,
         * and the cell is cacheable (it might not be if it's not globally visible), reuse the
         * previous return data to avoid repeatedly decoding items.
         */
        if (cbt->cip_saved == cip && F_ISSET(cbt, WT_CBT_CACHEABLE_RLE_CELL)) {
            F_CLR(&cbt->iface, WT_CURSTD_VALUE_EXT);
            F_SET(&cbt->iface, WT_CURSTD_VALUE_INT);
            cbt->iface.value.data = cbt->tmp->data;
            cbt->iface.value.size = cbt->tmp->size;
            return (0);
        }

        /* Unpack the cell and build the return information. */
        cell = WT_COL_PTR(page, cip);
        __wt_cell_unpack_kv(session, page->dsk, cell, &unpack);
        rle = __wt_cell_rle(&unpack);
        if (unpack.type == WT_CELL_DEL) {
            if (rle == 1) {
                ++*skippedp;
                continue;
            }

            /*
             * There can be huge gaps in the variable-length column-store name space appearing as
             * deleted records. If more than one deleted record, do the work of finding the next
             * record to return instead of looping through the records.
             *
             * First, find the smallest record in the update list that's larger than the current
             * record.
             */
            ins = __col_insert_search_gt(cbt->ins_head, cbt->recno);

            /*
             * Second, for records with RLEs greater than 1, the above call to __col_var_search
             * located this record in the page's list of repeating records, and returned the
             * starting record. The starting record plus the RLE is the record to which we could
             * skip, if there was no smaller record in the update list.
             */
            cbt->recno = rle_start + rle;
            if (ins != NULL && WT_INSERT_RECNO(ins) < cbt->recno)
                cbt->recno = WT_INSERT_RECNO(ins);

            /* Adjust for the outer loop increment. */
            --cbt->recno;
            ++*skippedp;
            continue;
        }

        /*
         * Read the on-disk value and/or history. Pass an update list: the update list may contain
         * the base update for a modify chain after rollback-to-stable, required for correctness.
         */
        WT_RET(__wt_txn_read(session, cbt, NULL, cbt->recno, cbt->ins ? cbt->ins->upd : NULL));
        if (cbt->upd_value->type == WT_UPDATE_INVALID ||
          cbt->upd_value->type == WT_UPDATE_TOMBSTONE) {
            ++*skippedp;
            continue;
        }
        __wt_value_return(cbt, cbt->upd_value);

        /*
         * It is only safe to cache the value for other keys in the same RLE cell if it is globally
         * visible. Otherwise, there might be some older timestamp where the value isn't uniform
         * across the cell. Always set cip_saved so it's easy to tell when we change cells.
         *
         * Note: it's important that we're checking the on-disk value for global visibility, and not
         * whatever __wt_txn_read returned, which might be something else. (If it's something else,
         * we can't cache it; but in that case the on-disk value cannot be globally visible.)
         */
        cbt->cip_saved = cip;
        if (rle > 1 &&
          __wt_txn_visible_all(session, unpack.tw.start_txn, unpack.tw.durable_start_ts)) {
            /*
             * Copy the value into cbt->tmp to cache it. This is perhaps unfortunate, because
             * copying isn't free, but it's currently necessary. The memory we're copying might be
             * on the disk page (which is safe because the page is pinned as long as the cursor is
             * sitting on it) but if not it belongs to cbt->upd_value, and that (though it belongs
             * to the cursor and won't disappear arbitrarily) might be invalidated or changed by
             * other paths through this function on a subsequent call but before we are done with
             * this RLE cell. In principle those paths could clear WT_CBT_CACHEABLE_RLE_CELL, but
             * the code is currently structured in a way that makes that difficult.
             */
            WT_RET(__wt_buf_set(session, cbt->tmp, cbt->iface.value.data, cbt->iface.value.size));
            F_SET(cbt, WT_CBT_CACHEABLE_RLE_CELL);
        } else
            F_CLR(cbt, WT_CBT_CACHEABLE_RLE_CELL);
        return (0);
    }
    /* NOTREACHED */
}

/*
 * __cursor_row_next --
 *     Move to the next row-store item.
 */
static inline int
__cursor_row_next(
  WT_CURSOR_BTREE *cbt, bool newpage, bool restart, size_t *skippedp, bool *key_out_of_boundsp)
{
    WT_CELL_UNPACK_KV kpack;
    WT_DECL_RET;
    WT_INSERT *ins;
    WT_ITEM *key;
    WT_PAGE *page;
    WT_ROW *rip;
    WT_SESSION_IMPL *session;

    key = &cbt->iface.key;
    page = cbt->ref->page;
    session = CUR2S(cbt);
    *key_out_of_boundsp = false;
    *skippedp = 0;

    /* If restarting after a prepare conflict, jump to the right spot. */
    if (restart) {
        if (cbt->iter_retry == WT_CBT_RETRY_INSERT)
            goto restart_read_insert;
        if (cbt->iter_retry == WT_CBT_RETRY_PAGE)
            goto restart_read_page;
    }
    cbt->iter_retry = WT_CBT_RETRY_NOTSET;

    /*
     * For row-store pages, we need a single item that tells us the part of the page we're walking
     * (otherwise switching from next to prev and vice-versa is just too complicated), so we map the
     * WT_ROW and WT_INSERT_HEAD insert array slots into a single name space: slot 1 is the
     * "smallest key insert list", slot 2 is WT_ROW[0], slot 3 is WT_INSERT_HEAD[0], and so on. This
     * means WT_INSERT lists are odd-numbered slots, and WT_ROW array slots are even-numbered slots.
     *
     * Initialize for each new page.
     */
    if (newpage) {
        /*
         * Be paranoid and set the slot out of bounds when moving to a new page.
         */
        cbt->slot = UINT32_MAX;
        cbt->ins_head = WT_ROW_INSERT_SMALLEST(page);
        cbt->ins = WT_SKIP_FIRST(cbt->ins_head);
        cbt->row_iteration_slot = 1;
        cbt->rip_saved = NULL;
        goto new_insert;
    }

    /* Move to the next entry and return the item. */
    for (;;) {
        /*
         * Continue traversing any insert list; maintain the insert list head reference and entry
         * count in case we switch to a cursor previous movement.
         */
        if (cbt->ins != NULL)
            cbt->ins = WT_SKIP_NEXT(cbt->ins);

new_insert:
        cbt->iter_retry = WT_CBT_RETRY_INSERT;
restart_read_insert:
        if ((ins = cbt->ins) != NULL) {
            key->data = WT_INSERT_KEY(ins);
            key->size = WT_INSERT_KEY_SIZE(ins);

            /*
             * If an upper bound has been set ensure that the key is within the range, otherwise
             * early exit.
             */
            if ((ret = __wt_btcur_bounds_early_exit(session, cbt, true, key_out_of_boundsp)) ==
              WT_NOTFOUND)
                WT_STAT_CONN_DATA_INCR(session, cursor_bounds_next_early_exit);
            WT_RET(ret);

            WT_RET(__wt_txn_read_upd_list(session, cbt, ins->upd));
            if (cbt->upd_value->type == WT_UPDATE_INVALID) {
                ++*skippedp;
                continue;
            }
            if (cbt->upd_value->type == WT_UPDATE_TOMBSTONE) {
                if (cbt->upd_value->tw.stop_txn != WT_TXN_NONE &&
                  __wt_txn_upd_value_visible_all(session, cbt->upd_value))
                    ++cbt->page_deleted_count;
                ++*skippedp;
                continue;
            }
            __wt_value_return(cbt, cbt->upd_value);
            return (0);
        }

        /* Check for the end of the page. */
        if (cbt->row_iteration_slot >= page->entries * 2 + 1)
            return (WT_NOTFOUND);
        ++cbt->row_iteration_slot;

        /*
         * Odd-numbered slots configure as WT_INSERT_HEAD entries, even-numbered slots configure as
         * WT_ROW entries.
         */
        if (cbt->row_iteration_slot & 0x01) {
            cbt->ins_head = WT_ROW_INSERT_SLOT(page, cbt->row_iteration_slot / 2 - 1);
            cbt->ins = WT_SKIP_FIRST(cbt->ins_head);
            goto new_insert;
        }
        cbt->ins_head = NULL;
        cbt->ins = NULL;

        cbt->iter_retry = WT_CBT_RETRY_PAGE;
        cbt->slot = cbt->row_iteration_slot / 2 - 1;
restart_read_page:
        rip = &page->pg_row[cbt->slot];
        /*
         * The saved cursor key from the slot is used later to match the prefix match or get the
         * value from the history store if the on-disk data is not visible.
         */
        WT_RET(__cursor_row_slot_key_return(cbt, rip, &kpack));

        /*
         * If an upper bound has been set ensure that the key is within the range, otherwise early
         * exit.
         */
        if ((ret = __wt_btcur_bounds_early_exit(session, cbt, true, key_out_of_boundsp)) ==
          WT_NOTFOUND)
            WT_STAT_CONN_DATA_INCR(session, cursor_bounds_next_early_exit);
        WT_RET(ret);

        /*
         * Read the on-disk value and/or history. Pass an update list: the update list may contain
         * the base update for a modify chain after rollback-to-stable, required for correctness.
         */
        WT_RET(
          __wt_txn_read(session, cbt, &cbt->iface.key, WT_RECNO_OOB, WT_ROW_UPDATE(page, rip)));
        if (cbt->upd_value->type == WT_UPDATE_INVALID) {
            ++*skippedp;
            continue;
        }
        if (cbt->upd_value->type == WT_UPDATE_TOMBSTONE) {
            if (cbt->upd_value->tw.stop_txn != WT_TXN_NONE &&
              __wt_txn_upd_value_visible_all(session, cbt->upd_value))
                ++cbt->page_deleted_count;
            ++*skippedp;
            continue;
        }
        __wt_value_return(cbt, cbt->upd_value);
        return (0);
    }
    /* NOTREACHED */
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __cursor_key_order_check_col --
 *     Check key ordering for column-store cursor movements.
 */
static int
__cursor_key_order_check_col(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, bool next)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    int cmp;

    WT_UNUSED(ret);
    btree = S2BT(session);
    cmp = 0; /* -Werror=maybe-uninitialized */

    if (cbt->lastrecno != WT_RECNO_OOB) {
        if (cbt->lastrecno < cbt->recno)
            cmp = -1;
        if (cbt->lastrecno > cbt->recno)
            cmp = 1;
    }

    if (cbt->lastrecno == WT_RECNO_OOB || (next && cmp < 0) || (!next && cmp > 0)) {
        cbt->lastrecno = cbt->recno;
        cbt->lastref = cbt->ref;
        cbt->lastslot = cbt->slot;
        cbt->lastins = cbt->ins;
        return (0);
    }

    WT_RET(__wt_msg(session, "dumping the tree"));
    WT_WITH_BTREE(session, btree, ret = __wt_debug_tree_all(session, NULL, NULL, NULL));
    __wt_verbose_error(session, WT_VERB_OUT_OF_ORDER,
      "WT_CURSOR.%s out-of-order returns: returned key %" PRIu64 " then key %" PRIu64,
      next ? "next" : "prev", cbt->lastrecno, cbt->recno);
    WT_ERR_PANIC(session, EINVAL, "found key out-of-order returns");

err:
    return (ret);
}

/*
 * __cursor_key_order_check_row --
 *     Check key ordering for row-store cursor movements.
 */
static int
__cursor_key_order_check_row(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, bool next)
{
    WT_BTREE *btree;
    WT_DECL_ITEM(a);
    WT_DECL_ITEM(b);
    WT_DECL_RET;
    WT_ITEM *key;
    int cmp;

    btree = S2BT(session);
    key = &cbt->iface.key;
    cmp = 0; /* -Werror=maybe-uninitialized */

    if (cbt->lastkey->size != 0)
        WT_RET(__wt_compare(session, btree->collator, cbt->lastkey, key, &cmp));

    if (cbt->lastkey->size == 0 || (next && cmp < 0) || (!next && cmp > 0)) {
        cbt->lastref = cbt->ref;
        cbt->lastslot = cbt->slot;
        cbt->lastins = cbt->ins;
        return (__wt_buf_set(session, cbt->lastkey, cbt->iface.key.data, cbt->iface.key.size));
    }

    WT_ERR(__wt_scr_alloc(session, 512, &a));
    WT_ERR(__wt_scr_alloc(session, 512, &b));

    __wt_verbose_error(session, WT_VERB_OUT_OF_ORDER,
      "WT_CURSOR.%s out-of-order returns: returned key %.1024s then key %.1024s",
      next ? "next" : "prev",
      __wt_buf_set_printable_format(
        session, cbt->lastkey->data, cbt->lastkey->size, btree->key_format, false, a),
      __wt_buf_set_printable_format(session, key->data, key->size, btree->key_format, false, b));
    WT_ERR(__wt_msg(session, "dumping the tree"));
    WT_WITH_BTREE(session, btree, ret = __wt_debug_tree_all(session, NULL, NULL, NULL));
    WT_ERR_PANIC(session, EINVAL, "found key out-of-order returns");

err:
    __wt_scr_free(session, &a);
    __wt_scr_free(session, &b);

    return (ret);
}

/*
 * __wt_cursor_key_order_check --
 *     Check key ordering for cursor movements.
 */
int
__wt_cursor_key_order_check(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, bool next)
{
    switch (cbt->ref->page->type) {
    case WT_PAGE_COL_FIX:
    case WT_PAGE_COL_VAR:
        return (__cursor_key_order_check_col(session, cbt, next));
    case WT_PAGE_ROW_LEAF:
        return (__cursor_key_order_check_row(session, cbt, next));
    default:
        return (__wt_illegal_value(session, cbt->ref->page->type));
    }
    /* NOTREACHED */
}

/*
 * __wt_cursor_key_order_init --
 *     Initialize key ordering checks for cursor movements after a successful search.
 */
int
__wt_cursor_key_order_init(WT_CURSOR_BTREE *cbt)
{
    WT_SESSION_IMPL *session;

    session = CUR2S(cbt);

    cbt->lastref = cbt->ref;
    cbt->lastslot = cbt->slot;
    cbt->lastins = cbt->ins;

    /*
     * Cursor searches set the position for cursor movements, set the last-key value for diagnostic
     * checking.
     */
    switch (cbt->ref->page->type) {
    case WT_PAGE_COL_FIX:
    case WT_PAGE_COL_VAR:
        cbt->lastrecno = cbt->recno;
        return (0);
    case WT_PAGE_ROW_LEAF:
        return (__wt_buf_set(session, cbt->lastkey, cbt->iface.key.data, cbt->iface.key.size));
    default:
        return (__wt_illegal_value(session, cbt->ref->page->type));
    }
    /* NOTREACHED */
}

/*
 * __wt_cursor_key_order_reset --
 *     Turn off key ordering checks for cursor movements.
 */
void
__wt_cursor_key_order_reset(WT_CURSOR_BTREE *cbt)
{
    /*
     * Clear the last-key returned, it doesn't apply.
     */
    if (cbt->lastkey != NULL)
        cbt->lastkey->size = 0;
    cbt->lastrecno = WT_RECNO_OOB;

    cbt->lastref = NULL;
    cbt->lastslot = UINT32_MAX;
    cbt->lastins = NULL;
}
#endif

/*
 * __wt_btcur_iterate_setup --
 *     Initialize a cursor for iteration, usually based on a search.
 */
void
__wt_btcur_iterate_setup(WT_CURSOR_BTREE *cbt)
{
    WT_PAGE *page;

    /*
     * We don't currently have to do any setup when we switch between next and prev calls, but I'm
     * sure we will someday -- I'm leaving support here for both flags for that reason.
     */
    F_SET(cbt, WT_CBT_ITERATE_NEXT | WT_CBT_ITERATE_PREV);

    /* Clear the count of deleted items on the page. */
    cbt->page_deleted_count = 0;

    /* Clear saved iteration cursor position information. */
    cbt->cip_saved = NULL;
    cbt->rip_saved = NULL;
    F_CLR(cbt, WT_CBT_CACHEABLE_RLE_CELL);

    /*
     * If we don't have a search page, then we're done, we're starting at the beginning or end of
     * the tree, not as a result of a search.
     */
    if (cbt->ref == NULL) {
#ifdef HAVE_DIAGNOSTIC
        __wt_cursor_key_order_reset(cbt);
#endif
        return;
    }

    page = cbt->ref->page;
    if (page->type == WT_PAGE_ROW_LEAF) {
        /*
         * For row-store pages, we need a single item that tells us the part of the page we're
         * walking (otherwise switching from next to prev and vice-versa is just too complicated),
         * so we map the WT_ROW and WT_INSERT_HEAD insert array slots into a single name space: slot
         * 1 is the "smallest key insert list", slot 2 is WT_ROW[0], slot 3 is WT_INSERT_HEAD[0],
         * and so on. This means WT_INSERT lists are odd-numbered slots, and WT_ROW array slots are
         * even-numbered slots.
         */
        cbt->row_iteration_slot = (cbt->slot + 1) * 2;
        if (cbt->ins_head != NULL) {
            if (cbt->ins_head == WT_ROW_INSERT_SMALLEST(page))
                cbt->row_iteration_slot = 1;
            else
                cbt->row_iteration_slot += 1;
        }
    } else {
        /*
         * For column-store pages, calculate the largest record on the page.
         */
        cbt->last_standard_recno = page->type == WT_PAGE_COL_VAR ? __col_var_last_recno(cbt->ref) :
                                                                   __col_fix_last_recno(cbt->ref);

        /* If we're traversing the append list, set the reference. */
        if (cbt->ins_head != NULL && cbt->ins_head == WT_COL_APPEND(page))
            F_SET(cbt, WT_CBT_ITERATE_APPEND);
    }
}

/*
 * __wt_btcur_next --
 *     Move to the next record in the tree.
 */
int
__wt_btcur_next(WT_CURSOR_BTREE *cbt, bool truncating)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_PAGE *page;
    WT_SESSION_IMPL *session;
    size_t total_skipped, skipped;
    uint32_t flags;
    bool key_out_of_bounds, newpage, restart, need_walk;
#ifdef HAVE_DIAGNOSTIC
    bool inclusive_set;

    inclusive_set = false;
#endif
    cursor = &cbt->iface;
    key_out_of_bounds = false;
    need_walk = false;
    session = CUR2S(cbt);
    total_skipped = 0;

    WT_STAT_CONN_DATA_INCR(session, cursor_next);

    flags = WT_READ_NO_SPLIT | WT_READ_SKIP_INTL; /* tree walk flags */
    if (truncating)
        LF_SET(WT_READ_TRUNCATE);

    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

    WT_ERR(__wt_cursor_func_init(cbt, false));

    /*
     * If we have a bound set we should position our cursor appropriately if it isn't already
     * positioned. It is possible that the positioning function can directly return the record. For
     * that to happen, the cursor must be placed on a valid record and must be positioned on the
     * first record within the bounds. If the record is not valid or is not positioned within the
     * bounds, continue the next traversal logic.
     */
    if (F_ISSET(cursor, WT_CURSTD_BOUND_LOWER) && !WT_CURSOR_IS_POSITIONED(cbt)) {
        WT_ERR(__wt_btcur_bounds_position(session, cbt, true, &need_walk));
        if (!need_walk) {
            __wt_value_return(cbt, cbt->upd_value);
            goto done;
        }
    }

    /*
     * If we aren't already iterating in the right direction, there's some setup to do.
     */
    if (!F_ISSET(cbt, WT_CBT_ITERATE_NEXT))
        __wt_btcur_iterate_setup(cbt);

    /*
     * Walk any page we're holding until the underlying call returns not-found. Then, move to the
     * next page, until we reach the end of the file.
     */
    restart = F_ISSET(cbt, WT_CBT_ITERATE_RETRY_NEXT);
    F_CLR(cbt, WT_CBT_ITERATE_RETRY_NEXT);
    for (newpage = false;; newpage = true, restart = false) {
        page = cbt->ref == NULL ? NULL : cbt->ref->page;

        if (F_ISSET(cbt, WT_CBT_ITERATE_APPEND)) {
            /* The page cannot be NULL if the above flag is set. */
            WT_ASSERT(session, page != NULL);
            switch (page->type) {
            case WT_PAGE_COL_FIX:
                ret = __cursor_fix_append_next(cbt, newpage, restart);
                break;
            case WT_PAGE_COL_VAR:
                ret = __cursor_var_append_next(cbt, newpage, restart, &skipped, &key_out_of_bounds);
                total_skipped += skipped;
                break;
            default:
                WT_ERR(__wt_illegal_value(session, page->type));
            }
            if (ret == 0 || ret == WT_PREPARE_CONFLICT)
                break;
            F_CLR(cbt, WT_CBT_ITERATE_APPEND);
            if (ret != WT_NOTFOUND)
                break;

            /*
             * If we are doing an operation when the cursor has bounds set, we need to check if we
             * have exited the next function due to the key being out of bounds. If so, we break
             * instead of walking onto the next page. We're not directly returning here to allow the
             * cursor to be reset first before we return WT_NOTFOUND.
             */
            if (key_out_of_bounds)
                break;
        } else if (page != NULL) {
            switch (page->type) {
            case WT_PAGE_COL_FIX:
                ret = __cursor_fix_next(cbt, newpage, restart);
                break;
            case WT_PAGE_COL_VAR:
                ret = __cursor_var_next(cbt, newpage, restart, &skipped, &key_out_of_bounds);
                total_skipped += skipped;
                break;
            case WT_PAGE_ROW_LEAF:
                ret = __cursor_row_next(cbt, newpage, restart, &skipped, &key_out_of_bounds);
                total_skipped += skipped;
                break;
            default:
                WT_ERR(__wt_illegal_value(session, page->type));
            }
            if (ret != WT_NOTFOUND)
                break;

            /*
             * If we are doing an operation when the cursor has bounds set, we need to check if we
             * have exited the next function due to the key being out of bounds. If so, we break
             * instead of walking onto the next page. We're not directly returning here to allow the
             * cursor to be reset first before we return WT_NOTFOUND.
             */
            if (key_out_of_bounds)
                break;

            /*
             * Column-store pages may have appended entries. Handle it separately from the usual
             * cursor code, it's in a simple format.
             */
            if (page->type != WT_PAGE_ROW_LEAF && (cbt->ins_head = WT_COL_APPEND(page)) != NULL) {
                F_SET(cbt, WT_CBT_ITERATE_APPEND);
                continue;
            }
        }
        /*
         * If we saw a lot of deleted records on this page, or we went all the way through a page
         * and only saw deleted records, try to evict the page when we release it. Otherwise
         * repeatedly deleting from the beginning of a tree can have quadratic performance. Take
         * care not to force eviction of pages that are genuinely empty, in new trees.
         *
         * A visible stop timestamp could have been treated as a tombstone and accounted in the
         * deleted count. Such a page might not have any new updates and be clean, but could benefit
         * from reconciliation getting rid of the obsolete content. Hence mark the page dirty to
         * force it through reconciliation.
         */
        if (page != NULL &&
          (cbt->page_deleted_count > WT_BTREE_DELETE_THRESHOLD ||
            (newpage && cbt->page_deleted_count > 0))) {
            WT_ERR(__wt_page_dirty_and_evict_soon(session, cbt->ref));
            WT_STAT_CONN_INCR(session, cache_eviction_force_delete);
        }
        cbt->page_deleted_count = 0;

        if (F_ISSET(cbt, WT_CBT_READ_ONCE))
            LF_SET(WT_READ_WONT_NEED);

        if (!F_ISSET(session->txn, WT_TXN_HAS_SNAPSHOT))
            LF_SET(WT_READ_VISIBLE_ALL);

        /*
         * If we are running with snapshot isolation, and not interested in returning tombstones, we
         * could potentially skip pages. The skip function looks at the aggregated timestamp
         * information to determine if something is visible on the page. If nothing is, the page is
         * skipped.
         */
        if (session->txn->isolation == WT_ISO_SNAPSHOT &&
          !F_ISSET(&cbt->iface, WT_CURSTD_IGNORE_TOMBSTONE))
            WT_ERR(
              __wt_tree_walk_custom_skip(session, &cbt->ref, __wt_btcur_skip_page, NULL, flags));
        else
            WT_ERR(__wt_tree_walk(session, &cbt->ref, flags));
        WT_ERR_TEST(cbt->ref == NULL, WT_NOTFOUND, false);
    }

done:
err:
    if (total_skipped != 0) {
        if (total_skipped < 100)
            WT_STAT_CONN_DATA_INCR(session, cursor_next_skip_lt_100);
        else
            WT_STAT_CONN_DATA_INCR(session, cursor_next_skip_ge_100);
    }

    WT_STAT_CONN_DATA_INCRV(session, cursor_next_skip_total, total_skipped);

    switch (ret) {
    case 0:
        F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
#ifdef HAVE_DIAGNOSTIC
        /*
         * Skip key order check, if prev is called after a next returned a prepare conflict error,
         * i.e cursor has changed direction at a prepared update, hence current key returned could
         * be same as earlier returned key.
         *
         * eg: Initial data set : (1,2,3,...10) insert key 11 in a prepare transaction. loop on next
         * will return 1,2,3...10 and subsequent call to next will return a prepare conflict. Now if
         * we call prev key 10 will be returned which will be same as earlier returned key.
         */
        if (!F_ISSET(cbt, WT_CBT_ITERATE_RETRY_PREV))
            ret = __wt_cursor_key_order_check(session, cbt, true);

        if (need_walk) {
            /*
             * The bounds positioning code relies on the assumption that if we had to walk then we
             * can't possibly have walked to the lower bound. We check that assumption here by
             * comparing the lower bound with our current key or recno. Force inclusive to be false
             * so we don't consider the bound itself.
             */
            inclusive_set = F_ISSET(cursor, WT_CURSTD_BOUND_LOWER_INCLUSIVE);
            F_CLR(cursor, WT_CURSTD_BOUND_LOWER_INCLUSIVE);
            ret = __wt_compare_bounds(
              session, cursor, &cbt->iface.key, cbt->recno, false, &key_out_of_bounds);
            WT_ASSERT(session, ret == 0 && !key_out_of_bounds);
            if (inclusive_set)
                F_SET(cursor, WT_CURSTD_BOUND_LOWER_INCLUSIVE);
        }
#endif
        break;
    case WT_PREPARE_CONFLICT:
        /*
         * If prepare conflict occurs, cursor should not be reset, as current cursor position will
         * be reused in case of a retry from user.
         */
        F_SET(cbt, WT_CBT_ITERATE_RETRY_NEXT);
        break;
    default:
        WT_TRET(__cursor_reset(cbt));
    }
    F_CLR(cbt, WT_CBT_ITERATE_RETRY_PREV);

    if (ret == 0)
        WT_RET(__wt_btcur_evict_reposition(cbt));

    return (ret);
}
