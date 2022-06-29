/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Walking backwards through skip lists.
 *
 * The skip list stack is an array of pointers set up by a search. It points to the position a node
 * should go in the skip list. In other words, the skip list search stack always points *after* the
 * search item (that is, into the search item's next array).
 *
 * Helper macros to go from a stack pointer at level i, pointing into a next array, back to the
 * insert node containing that next array.
 */
#undef PREV_ITEM
#define PREV_ITEM(ins_head, insp, i)                      \
    (((insp) == &(ins_head)->head[i] || (insp) == NULL) ? \
        NULL :                                            \
        (WT_INSERT *)((char *)((insp) - (i)) - offsetof(WT_INSERT, next)))

#undef PREV_INS
#define PREV_INS(cbt, i) PREV_ITEM((cbt)->ins_head, (cbt)->ins_stack[(i)], (i))

/*
 * __cursor_skip_prev --
 *     Move back one position in a skip list stack (aka "finger").
 */
static inline int
__cursor_skip_prev(WT_CURSOR_BTREE *cbt)
{
    WT_INSERT *current, *ins;
    WT_ITEM key;
    WT_SESSION_IMPL *session;
    uint64_t recno;
    int i;

    session = CUR2S(cbt);

restart:
    /*
     * If the search stack does not point at the current item, fill it in with a search.
     */
    recno = WT_INSERT_RECNO(cbt->ins);
    while ((current = cbt->ins) != PREV_INS(cbt, 0)) {
        if (CUR2BT(cbt)->type == BTREE_ROW) {
            key.data = WT_INSERT_KEY(current);
            key.size = WT_INSERT_KEY_SIZE(current);
            WT_RET(__wt_search_insert(session, cbt, cbt->ins_head, &key));
        } else
            cbt->ins = __col_insert_search(cbt->ins_head, cbt->ins_stack, cbt->next_stack, recno);
    }

    /*
     * Find the first node up the search stack that does not move.
     *
     * The depth of the current item must be at least this level, since we
     * see it in that many levels of the stack.
     *
     * !!! Watch these loops carefully: they all rely on the value of i,
     * and the exit conditions to end up with the right values are
     * non-trivial.
     */
    ins = NULL; /* -Wconditional-uninitialized */
    for (i = 0; i < WT_SKIP_MAXDEPTH - 1; i++)
        if ((ins = PREV_INS(cbt, i + 1)) != current)
            break;

    /*
     * Find a starting point for the new search. That is either at the non-moving node if we found a
     * valid node, or the beginning of the next list down that is not the current node.
     *
     * Since it is the beginning of a list, and we know the current node is has a skip depth at
     * least this high, any node we find must sort before the current node.
     */
    if (ins == NULL || ins == current)
        for (; i >= 0; i--) {
            cbt->ins_stack[i] = NULL;
            cbt->next_stack[i] = NULL;
            ins = cbt->ins_head->head[i];
            if (ins != NULL && ins != current)
                break;
        }

    /* Walk any remaining levels until just before the current node. */
    while (i >= 0) {
        /*
         * If we get to the end of a list without finding the current item, we must have raced with
         * an insert. Restart the search.
         */
        if (ins == NULL) {
            cbt->ins_stack[0] = NULL;
            cbt->next_stack[0] = NULL;
            goto restart;
        }
        if (ins->next[i] != current) /* Stay at this level */
            ins = ins->next[i];
        else { /* Drop down a level */
            cbt->ins_stack[i] = &ins->next[i];
            cbt->next_stack[i] = ins->next[i];
            --i;
        }
    }

    /* If we found a previous node, the next one must be current. */
    if (cbt->ins_stack[0] != NULL && *cbt->ins_stack[0] != current)
        goto restart;

    cbt->ins = PREV_INS(cbt, 0);
    return (0);
}

/*
 * __cursor_fix_append_prev --
 *     Return the previous fixed-length entry on the append list.
 */
static inline int
__cursor_fix_append_prev(WT_CURSOR_BTREE *cbt, bool newpage, bool restart)
{
    WT_SESSION_IMPL *session;

    session = CUR2S(cbt);

    /* If restarting after a prepare conflict, jump to the right spot. */
    if (restart)
        goto restart_read;

    if (newpage) {
        if ((cbt->ins = WT_SKIP_LAST(cbt->ins_head)) == NULL)
            return (WT_NOTFOUND);
    } else {
        /* Move to the previous record in the append list, if any. */
        if (cbt->ins != NULL && cbt->recno <= WT_INSERT_RECNO(cbt->ins))
            WT_RET(__cursor_skip_prev(cbt));

        /*
         * Handle the special case of leading implicit records, that is,
         * there aren't any records in the page not on the append list,
         * and the append list's first record isn't the first record on
         * the page. (Although implemented as a test of the page values,
         * this is really a test for a tree where the first inserted
         * record wasn't record 1, any other page with only an append
         * list will have a first page record number matching the first
         * record in the append list.)
         *
         * The "right" place to handle this is probably in our caller.
         * The high-level cursor-previous routine would:
         *    -- call this routine to walk the append list
         *    -- call the routine to walk the standard page items
         *    -- call the tree walk routine looking for a previous page
         * Each of them returns WT_NOTFOUND, at which point our caller
         * checks the cursor record number, and if it's larger than 1,
         * returns the implicit records.  Instead, I'm trying to detect
         * the case here, mostly because I don't want to put that code
         * into our caller.  Anyway, if this code breaks for any reason,
         * that's the way I'd go.
         *
         * If we're not pointing to a WT_INSERT entry (we didn't find a
         * WT_INSERT record preceding our record name-space), check if
         * we've reached the beginning of this page, a possibility if a
         * page had a large number of items appended, and then split.
         * If not, check if there are any records on the page. If there
         * aren't, then we're in the magic zone, keep going until we get
         * to a record number matching the first record on the page.
         */
        if (cbt->ins == NULL &&
          (cbt->recno == cbt->ref->ref_recno || __col_fix_last_recno(cbt->ref) != 0))
            return (WT_NOTFOUND);
    }

    /*
     * This code looks different from the cursor-next code. The append list may be preceded by other
     * rows. If we're iterating through the tree, starting at the last record in the tree, by
     * definition we're starting a new iteration and we set the record number to the last record
     * found on the page. Otherwise, decrement the record.
     */
    if (newpage)
        __cursor_set_recno(cbt, WT_INSERT_RECNO(cbt->ins));
    else
        __cursor_set_recno(cbt, cbt->recno - 1);

    if (F_ISSET(&cbt->iface, WT_CURSTD_KEY_ONLY))
        return (0);

    /*
     * Fixed-width column store appends are inherently non-transactional. Even a non-visible update
     * by a concurrent or aborted transaction changes the effective end of the data. The effect is
     * subtle because of the blurring between deleted and empty values, but ideally we would skip
     * all uncommitted changes at the end of the data. This doesn't apply to variable-width column
     * stores because the implicitly created records written by reconciliation are deleted and so
     * can be never seen by a read.
     */
    if (cbt->ins == NULL || cbt->recno > WT_INSERT_RECNO(cbt->ins)) {
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
 * __cursor_fix_prev --
 *     Move to the previous, fixed-length column-store item.
 */
static inline int
__cursor_fix_prev(WT_CURSOR_BTREE *cbt, bool newpage, bool restart)
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
        __cursor_set_recno(cbt, cbt->last_standard_recno);
        goto new_page;
    }

    /* Move to the previous entry and return the item. */
    if (cbt->recno == cbt->ref->ref_recno)
        return (WT_NOTFOUND);
    __cursor_set_recno(cbt, cbt->recno - 1);

new_page:
restart_read:
    /* We only have one slot. */
    cbt->slot = 0;

    /* Check any insert list for a matching record. */
    cbt->ins_head = WT_COL_UPDATE_SINGLE(page);
    cbt->ins = __col_insert_search(cbt->ins_head, cbt->ins_stack, cbt->next_stack, cbt->recno);
    if (cbt->ins != NULL && cbt->recno != WT_INSERT_RECNO(cbt->ins))
        cbt->ins = NULL;

    if (F_ISSET(&cbt->iface, WT_CURSTD_KEY_ONLY))
        return (0);

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
 * __cursor_var_append_prev --
 *     Return the previous variable-length entry on the append list.
 */
static inline int
__cursor_var_append_prev(WT_CURSOR_BTREE *cbt, bool newpage, bool restart, size_t *skippedp)
{
    WT_SESSION_IMPL *session;

    session = CUR2S(cbt);
    *skippedp = 0;

    /* If restarting after a prepare conflict, jump to the right spot. */
    if (restart)
        goto restart_read;

    if (newpage) {
        cbt->ins = WT_SKIP_LAST(cbt->ins_head);
        goto new_page;
    }

    for (;;) {
        WT_RET(__cursor_skip_prev(cbt));
new_page:
        if (cbt->ins == NULL)
            return (WT_NOTFOUND);

        __cursor_set_recno(cbt, WT_INSERT_RECNO(cbt->ins));

        if (F_ISSET(&cbt->iface, WT_CURSTD_KEY_ONLY))
            return (0);

restart_read:
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
 * __cursor_var_prev --
 *     Move to the previous, variable-length column-store item.
 */
static inline int
__cursor_var_prev(WT_CURSOR_BTREE *cbt, bool newpage, bool restart, size_t *skippedp)
{
    WT_CELL *cell;
    WT_CELL_UNPACK_KV unpack;
    WT_COL *cip;
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
        __cursor_set_recno(cbt, cbt->last_standard_recno);
        cbt->cip_saved = NULL;
        F_CLR(cbt, WT_CBT_CACHEABLE_RLE_CELL);
        goto new_page;
    }

    /* Move to the previous entry and return the item. */
    for (;;) {
        __cursor_set_recno(cbt, cbt->recno - 1);

new_page:
        if (cbt->recno < cbt->ref->ref_recno)
            return (WT_NOTFOUND);

restart_read:
        /* Find the matching WT_COL slot. */
        if ((cip = __col_var_search(cbt->ref, cbt->recno, &rle_start)) == NULL)
            return (WT_NOTFOUND);
        cbt->slot = WT_COL_SLOT(page, cip);

        /* Check any insert list for a matching record. */
        cbt->ins_head = WT_COL_UPDATE_SLOT(page, cbt->slot);
        cbt->ins = __col_insert_search_match(cbt->ins_head, cbt->recno);
        __wt_upd_value_clear(cbt->upd_value);
        if (cbt->ins != NULL) {
            if (F_ISSET(&cbt->iface, WT_CURSTD_KEY_ONLY))
                return (0);

            WT_RET(__wt_txn_read_upd_list(session, cbt, cbt->ins->upd));
        }
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
             * First, find the largest record in the update list that's smaller than the current
             * record.
             */
            ins = __col_insert_search_lt(cbt->ins_head, cbt->recno);

            /*
             * Second, for records with RLEs greater than 1, the above call to __col_var_search
             * located this record in the page's list of repeating records, and returned the
             * starting record. The starting record - 1 is the record to which we could skip, if
             * there was no larger record in the update list.
             */
            cbt->recno = rle_start - 1;
            if (ins != NULL && WT_INSERT_RECNO(ins) > cbt->recno)
                cbt->recno = WT_INSERT_RECNO(ins);

            /* Adjust for the outer loop decrement. */
            ++cbt->recno;
            ++*skippedp;
            continue;
        }

        if (F_ISSET(&cbt->iface, WT_CURSTD_KEY_ONLY))
            return (0);

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
 * __cursor_row_prev --
 *     Move to the previous row-store item.
 */
static inline int
__cursor_row_prev(
  WT_CURSOR_BTREE *cbt, bool newpage, bool restart, size_t *skippedp, bool *key_out_of_bounds)
{
    WT_CELL_UNPACK_KV kpack;
    WT_INSERT *ins;
    WT_ITEM *key;
    WT_PAGE *page;
    WT_ROW *rip;
    WT_SESSION_IMPL *session;

    key = &cbt->iface.key;
    page = cbt->ref->page;
    session = CUR2S(cbt);
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
        /* Check if keys need to be instantiated before we walk the page. */
        WT_RET(__wt_row_leaf_key_instantiate(session, page));

        /*
         * Be paranoid and set the slot out of bounds when moving to a new page.
         */
        cbt->slot = UINT32_MAX;
        if (page->entries == 0)
            cbt->ins_head = WT_ROW_INSERT_SMALLEST(page);
        else
            cbt->ins_head = WT_ROW_INSERT_SLOT(page, page->entries - 1);
        cbt->ins = WT_SKIP_LAST(cbt->ins_head);
        cbt->row_iteration_slot = page->entries * 2 + 1;
        cbt->rip_saved = NULL;
        goto new_insert;
    }

    /* Move to the previous entry and return the item. */
    for (;;) {
        /*
         * Continue traversing any insert list. Maintain the reference to the current insert element
         * in case we switch to a cursor next movement.
         */
        if (cbt->ins != NULL)
            WT_RET(__cursor_skip_prev(cbt));

new_insert:
        cbt->iter_retry = WT_CBT_RETRY_INSERT;
restart_read_insert:
        if ((ins = cbt->ins) != NULL) {
            key->data = WT_INSERT_KEY(ins);
            key->size = WT_INSERT_KEY_SIZE(ins);

            if (F_ISSET(&cbt->iface, WT_CURSTD_KEY_ONLY))
                return (0);

            /*
             * If a lower bound has been set ensure that the key is within the range, otherwise
             * early exit.
             */
            if (F_ISSET(&cbt->iface, WT_CURSTD_BOUND_LOWER))
                WT_RET(__wt_btcur_bounds_early_exit(session, cbt, false, key_out_of_bounds));

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

        /* Check for the beginning of the page. */
        if (cbt->row_iteration_slot == 1)
            return (WT_NOTFOUND);
        --cbt->row_iteration_slot;

        /*
         * Odd-numbered slots configure as WT_INSERT_HEAD entries, even-numbered slots configure as
         * WT_ROW entries.
         */
        if (cbt->row_iteration_slot & 0x01) {
            cbt->ins_head = cbt->row_iteration_slot == 1 ?
              WT_ROW_INSERT_SMALLEST(page) :
              WT_ROW_INSERT_SLOT(page, cbt->row_iteration_slot / 2 - 1);
            cbt->ins = WT_SKIP_LAST(cbt->ins_head);
            goto new_insert;
        }
        cbt->ins_head = NULL;
        cbt->ins = NULL;

        cbt->iter_retry = WT_CBT_RETRY_PAGE;
        cbt->slot = cbt->row_iteration_slot / 2 - 1;
restart_read_page:
        rip = &page->pg_row[cbt->slot];
        /*
         * The saved cursor key from the slot is used later to get the value from the history store
         * if the on-disk data is not visible.
         */
        WT_RET(__cursor_row_slot_key_return(cbt, rip, &kpack));

        if (F_ISSET(&cbt->iface, WT_CURSTD_KEY_ONLY))
            return (0);

        /*
         * If a lower bound has been set ensure that the key is within the range, otherwise early
         * exit.
         */
        if (F_ISSET(&cbt->iface, WT_CURSTD_BOUND_LOWER))
            WT_RET(__wt_btcur_bounds_early_exit(session, cbt, false, key_out_of_bounds));
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

/*
 * __wt_btcur_prev --
 *     Move to the previous record in the tree.
 */
int
__wt_btcur_prev(WT_CURSOR_BTREE *cbt, bool truncating)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_PAGE *page;
    WT_SESSION_IMPL *session;
    size_t total_skipped, skipped;
    uint32_t flags;
    bool key_out_of_bounds, newpage, restart, need_walk;

    cursor = &cbt->iface;
    key_out_of_bounds = false;
    need_walk = false;
    session = CUR2S(cbt);
    total_skipped = 0;

    /*
     * If we have a bound set we should position our cursor appropriately if it isn't already
     * positioned.
     *
     * In one scenario this function returns and needs to walk to the next record. In that case we
     * set a boolean. We need to be careful that the entry flag isn't set so we don't re-enter
     * search near.
     *
     * This logic must come before the call to __wt_cursor_func_init, as it will set the cbt active
     * flag which changes the logic flow within the subsequent search near resulting in a
     * segmentation fault. It is better to not set state prior to calling search near. Additionally
     * the cursor initialization function does take a snapshot if required as does cursor search
     * near.
     */
    if (F_ISSET(cursor, WT_CURSTD_BOUND_UPPER) && !WT_CURSOR_IS_POSITIONED(cbt) &&
      !F_ISSET(cursor, WT_CURSTD_BOUND_ENTRY)) {
        WT_ERR(__wt_btcur_bounds_row_position(session, cbt, false, &need_walk));
        if (!need_walk)
            return (0);
    }
    WT_STAT_CONN_DATA_INCR(session, cursor_prev);

    /* tree walk flags */
    flags = WT_READ_NO_SPLIT | WT_READ_PREV | WT_READ_SKIP_INTL;
    if (truncating)
        LF_SET(WT_READ_TRUNCATE);

    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

    WT_ERR(__wt_cursor_func_init(cbt, false));
    /*
     * If we aren't already iterating in the right direction, there's some setup to do.
     */
    if (!F_ISSET(cbt, WT_CBT_ITERATE_PREV))
        __wt_btcur_iterate_setup(cbt);

    /*
     * Walk any page we're holding until the underlying call returns not-found. Then, move to the
     * previous page, until we reach the start of the file.
     */
    restart = F_ISSET(cbt, WT_CBT_ITERATE_RETRY_PREV);
    F_CLR(cbt, WT_CBT_ITERATE_RETRY_PREV);
    for (newpage = false;; newpage = true, restart = false) {
        /* Calls with key only flag should never restart. */
        WT_ASSERT(session, !F_ISSET(&cbt->iface, WT_CURSTD_KEY_ONLY) || !restart);
        page = cbt->ref == NULL ? NULL : cbt->ref->page;

        /*
         * Column-store pages may have appended entries. Handle it separately from the usual cursor
         * code, it's in a simple format.
         */
        if (newpage && page != NULL && page->type != WT_PAGE_ROW_LEAF &&
          (cbt->ins_head = WT_COL_APPEND(page)) != NULL)
            F_SET(cbt, WT_CBT_ITERATE_APPEND);

        if (F_ISSET(cbt, WT_CBT_ITERATE_APPEND)) {
            /* The page cannot be NULL if the above flag is set. */
            WT_ASSERT(session, page != NULL);
            switch (page->type) {
            case WT_PAGE_COL_FIX:
                ret = __cursor_fix_append_prev(cbt, newpage, restart);
                break;
            case WT_PAGE_COL_VAR:
                ret = __cursor_var_append_prev(cbt, newpage, restart, &skipped);
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
            newpage = true;
        }
        if (page != NULL) {
            switch (page->type) {
            case WT_PAGE_COL_FIX:
                ret = __cursor_fix_prev(cbt, newpage, restart);
                break;
            case WT_PAGE_COL_VAR:
                ret = __cursor_var_prev(cbt, newpage, restart, &skipped);
                total_skipped += skipped;
                break;
            case WT_PAGE_ROW_LEAF:
                ret = __cursor_row_prev(cbt, newpage, restart, &skipped, &key_out_of_bounds);
                total_skipped += skipped;
                break;
            default:
                WT_ERR(__wt_illegal_value(session, page->type));
            }
            if (ret != WT_NOTFOUND)
                break;
            /*
             * If we are doing a search near with prefix key configured or the cursor has bounds
             * set, we need to check if we have exited the prev function due to a prefix key
             * mismatch or the key is out of bounds. If so, we break instead of walking onto the
             * next page. We're not directly returning here to allow the cursor to be reset first
             * before we return WT_NOTFOUND.
             */
            if (key_out_of_bounds)
                break;
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
        if (!F_ISSET(&cbt->iface, WT_CURSTD_KEY_ONLY) &&
          session->txn->isolation == WT_ISO_SNAPSHOT &&
          !F_ISSET(&cbt->iface, WT_CURSTD_IGNORE_TOMBSTONE))
            WT_ERR(
              __wt_tree_walk_custom_skip(session, &cbt->ref, __wt_btcur_skip_page, NULL, flags));
        else
            WT_ERR(__wt_tree_walk(session, &cbt->ref, flags));
        WT_ERR_TEST(cbt->ref == NULL, WT_NOTFOUND, false);
    }

err:
    if (total_skipped < 100)
        WT_STAT_CONN_DATA_INCR(session, cursor_prev_skip_lt_100);
    else
        WT_STAT_CONN_DATA_INCR(session, cursor_prev_skip_ge_100);

    WT_STAT_CONN_DATA_INCRV(session, cursor_prev_skip_total, total_skipped);

    switch (ret) {
    case 0:
        if (F_ISSET(&cbt->iface, WT_CURSTD_KEY_ONLY))
            F_SET(cursor, WT_CURSTD_KEY_INT);
        else
            F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
#ifdef HAVE_DIAGNOSTIC
        /*
         * Skip key order check, if next is called after a prev returned a prepare conflict error,
         * i.e cursor has changed direction at a prepared update, hence current key returned could
         * be same as earlier returned key.
         *
         * eg: Initial data set : (2,3,...10) insert key 1 in a prepare transaction. loop on prev
         * will return 10,...3,2 and subsequent call to prev will return a prepare conflict. Now if
         * we call next key 2 will be returned which will be same as earlier returned key.
         */
        if (!F_ISSET(cbt, WT_CBT_ITERATE_RETRY_NEXT))
            ret = __wt_cursor_key_order_check(session, cbt, false);
#endif
        break;
    case WT_PREPARE_CONFLICT:
        /*
         * If prepare conflict occurs, cursor should not be reset, as current cursor position will
         * be reused in case of a retry from user.
         */
        F_SET(cbt, WT_CBT_ITERATE_RETRY_PREV);
        break;
    default:
        WT_TRET(__cursor_reset(cbt));
    }
    F_CLR(cbt, WT_CBT_ITERATE_RETRY_NEXT);

    if (ret == 0)
        WT_RET(__wt_btcur_evict_reposition(cbt));

    return (ret);
}
