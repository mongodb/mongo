/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_page_modify_alloc --
 *     Allocate a page's modification structure.
 */
int
__wt_page_modify_alloc(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_DECL_RET;
    WT_PAGE_MODIFY *modify;

    WT_RET(__wt_calloc_one(session, &modify));

    /* Initialize the spinlock for the page. */
    WT_SPIN_INIT_TRACKED(session, &modify->page_lock, btree_page);

    /*
     * Multiple threads of control may be searching and deciding to modify a page. If our modify
     * structure is used, update the page's memory footprint, else discard the modify structure,
     * another thread did the work.
     */
    if (__wt_atomic_cas_ptr(&page->modify, NULL, modify))
        __wt_cache_page_inmem_incr(session, page, sizeof(*modify), false);
    else
err:
        __wt_free(session, modify);
    return (ret);
}

/*
 * __row_insert_alloc --
 *     Row-store insert: allocate a WT_INSERT structure and fill it in.
 */
static int
__row_insert_alloc(WT_SESSION_IMPL *session, const WT_ITEM *key, u_int skipdepth, WT_INSERT **insp,
  size_t *ins_sizep)
{
    WT_INSERT *ins;
    size_t ins_size;

    /*
     * Allocate the WT_INSERT structure, next pointers for the skip list, and room for the key. Then
     * copy the key into place.
     */
    ins_size = sizeof(WT_INSERT) + skipdepth * sizeof(WT_INSERT *) + key->size;
    WT_RET(__wt_calloc(session, 1, ins_size, &ins));

    ins->u.key.offset = WT_STORE_SIZE(ins_size - key->size);
    WT_INSERT_KEY_SIZE(ins) = WT_STORE_SIZE(key->size);
    memcpy(WT_INSERT_KEY(ins), key->data, key->size);

    *insp = ins;
    if (ins_sizep != NULL)
        *ins_sizep = ins_size;
    return (0);
}

/*
 * __wt_row_modify --
 *     Row-store insert, update and delete.
 */
int
__wt_row_modify(WT_CURSOR_BTREE *cbt, const WT_ITEM *key, const WT_ITEM *value,
  WT_UPDATE **updp_arg, u_int modify_type, bool exclusive, bool restore)
{
    WT_DECL_RET;
    WT_INSERT *ins;
    WT_INSERT_HEAD *ins_head, **ins_headp;
    WT_PAGE *page;
    WT_PAGE_MODIFY *mod;
    WT_SESSION_IMPL *session;
    WT_UPDATE *last_upd, *old_upd, *upd, *upd_arg, **upd_entry;
    wt_timestamp_t prev_upd_ts;
    size_t ins_size, upd_size;
    uint32_t ins_slot;
    u_int i, skipdepth;
    bool added_to_txn, inserted_to_update_chain;

    ins = NULL;
    page = cbt->ref->page;
    session = CUR2S(cbt);
    last_upd = NULL;
    upd_arg = updp_arg == NULL ? NULL : *updp_arg;
    upd = upd_arg;
    prev_upd_ts = WT_TS_NONE;
    added_to_txn = inserted_to_update_chain = false;
    upd_size = 0;

    /*
     * We should have one of the following:
     * - A full update list to instantiate.
     * - An update to append to the existing update list.
     * - A key/value pair to create an update with and append to the update list.
     * - A key with no value to create a reserved or tombstone update to append to the update list.
     *
     * A "full update list" is distinguished from "an update" by checking whether it has a "next"
     * update. The modify type should only be set if no update list provided.
     */
    WT_ASSERT(session,
      ((modify_type == WT_UPDATE_RESERVE || modify_type == WT_UPDATE_TOMBSTONE) && value == NULL &&
        upd_arg == NULL) ||
        (!(modify_type == WT_UPDATE_RESERVE || modify_type == WT_UPDATE_TOMBSTONE) &&
          ((value == NULL && upd_arg != NULL) || (value != NULL && upd_arg == NULL))));
    WT_ASSERT(session, upd_arg == NULL || modify_type == WT_UPDATE_INVALID);

    /* If we don't yet have a modify structure, we'll need one. */
    WT_RET(__wt_page_modify_init(session, page));
    mod = page->modify;

    /*
     * Modify: allocate an update array as necessary, build a WT_UPDATE structure, and call a
     * serialized function to insert the WT_UPDATE structure.
     *
     * Insert: allocate an insert array as necessary, build a WT_INSERT and WT_UPDATE structure
     * pair, and call a serialized function to insert the WT_INSERT structure.
     */
    if (cbt->compare == 0) {
        if (cbt->ins == NULL) {
            /* Allocate an update array as necessary. */
            WT_PAGE_ALLOC_AND_SWAP(session, page, mod->mod_row_update, upd_entry, page->entries);

            /* Set the WT_UPDATE array reference. */
            upd_entry = &mod->mod_row_update[cbt->slot];
        } else
            upd_entry = &cbt->ins->upd;

        if (upd_arg == NULL) {
            /* Make sure the modify can proceed. */
            WT_ERR(
              __wt_txn_modify_check(session, cbt, old_upd = *upd_entry, &prev_upd_ts, modify_type));

            /* Allocate a WT_UPDATE structure and transaction ID. */
            WT_ERR(__wt_upd_alloc(session, value, modify_type, &upd, &upd_size));
            upd->prev_durable_ts = prev_upd_ts;
            WT_ERR(__wt_txn_modify(session, upd));
            added_to_txn = true;

            /* Avoid WT_CURSOR.update data copy. */
            __wt_upd_value_assign(cbt->modify_update, upd);
        } else {
            /*
             * We only update history store records in two cases:
             *  1) Delete the record with a tombstone with WT_TS_NONE.
             *  2) Reinsert an update that has been deleted by a prepared commit or rollback.
             */
            WT_ASSERT(session,
              !WT_IS_HS(S2BT(session)->dhandle) || *upd_entry == NULL ||
                (*upd_entry)->next == NULL ||
                ((*upd_entry)->type == WT_UPDATE_STANDARD &&
                  (*upd_entry)->txnid != WT_TXN_ABORTED) ||
                ((*upd_entry)->type == WT_UPDATE_TOMBSTONE && (*upd_entry)->txnid == WT_TXN_NONE &&
                  (*upd_entry)->upd_start_ts == WT_TS_NONE) ||
                ((*upd_entry)->txnid == WT_TXN_ABORTED &&
                  (*upd_entry)->next->txnid == WT_TXN_ABORTED) ||
                ((*upd_entry)->type == WT_UPDATE_TOMBSTONE &&
                  (*upd_entry)->txnid != WT_TXN_ABORTED &&
                  (*upd_entry)->next->type == WT_UPDATE_STANDARD &&
                  (*upd_entry)->next->txnid != WT_TXN_ABORTED));
            WT_ASSERT(session,
              !WT_IS_HS(S2BT(session)->dhandle) ||
                (upd_arg->type == WT_UPDATE_TOMBSTONE && upd_arg->upd_start_ts == WT_TS_NONE &&
                  upd_arg->next == NULL) ||
                (upd_arg->type == WT_UPDATE_TOMBSTONE && upd_arg->next != NULL &&
                  upd_arg->next->type == WT_UPDATE_STANDARD && upd_arg->next->next == NULL) ||
                (upd_arg->type == WT_UPDATE_STANDARD && upd_arg->next == NULL));

            upd_size = __wt_update_list_memsize(upd);

            /* If there are existing updates, append them after the new updates. */
            for (last_upd = upd; last_upd->next != NULL; last_upd = last_upd->next)
                ;
            last_upd->next = *upd_entry;

            /*
             * If we restore an update chain in update restore eviction, there should be no update
             * or a restored tombstone on the existing update chain except for btrees with leaf
             * delta enabled or a prepared update if the preserve prepared config is enabled.
             * FIXME-WT-14885: No need to consider the delta case if we have implemented delta
             * consolidation.
             */
            WT_ASSERT_ALWAYS(session,
              !restore ||
                (*upd_entry == NULL ||
                  (WT_DELTA_LEAF_ENABLED(session) && (*upd_entry)->type == WT_UPDATE_TOMBSTONE &&
                    F_ISSET(*upd_entry, WT_UPDATE_RESTORED_FROM_DS)) ||
                  (F_ISSET(S2C(session), WT_CONN_PRESERVE_PREPARED) &&
                    F_ISSET(*upd_entry, WT_UPDATE_PREPARE_RESTORED_FROM_DS))),
              "Illegal update on chain during update restore eviction");

            /*
             * We can either put multiple new updates or a single update on the update chain.
             *
             * Set the "old" entry to the second update in the list so that the serialization
             * function succeeds in swapping the first update into place.
             */
            if (upd->next != NULL)
                *upd_entry = upd->next;
            old_upd = *upd_entry;
        }

        /*
         * Point the new WT_UPDATE item to the next element in the list. If we get it right, the
         * serialization function lock acts as our memory barrier to flush this write.
         */
        upd->next = old_upd;

        /* Serialize the update. */
        WT_ERR(__wt_update_serial(session, cbt, page, upd_entry, &upd, upd_size, exclusive));
    } else {
        /*
         * Allocate the insert array as necessary.
         *
         * We allocate an additional insert array slot for insert keys sorting less than any key on
         * the page. The test to select that slot is baroque: if the search returned the first page
         * slot, we didn't end up processing an insert list, and the comparison value indicates the
         * search key was smaller than the returned slot, then we're using the smallest-key insert
         * slot. That's hard, so we set a flag.
         */
        WT_PAGE_ALLOC_AND_SWAP(session, page, mod->mod_row_insert, ins_headp, page->entries + 1);
        ins_slot = F_ISSET(cbt, WT_CBT_SEARCH_SMALLEST) ? page->entries : cbt->slot;
        ins_headp = &mod->mod_row_insert[ins_slot];

        /* Allocate the WT_INSERT_HEAD structure as necessary. */
        WT_PAGE_ALLOC_AND_SWAP(session, page, *ins_headp, ins_head, 1);
        ins_head = *ins_headp;

        /* Choose a skiplist depth for this insert. */
        skipdepth = __wt_skip_choose_depth(session);

        /*
         * Allocate a WT_INSERT/WT_UPDATE pair and transaction ID, and update the cursor to
         * reference it (the WT_INSERT_HEAD might be allocated, the WT_INSERT was allocated).
         */
        WT_ERR(__row_insert_alloc(session, key, skipdepth, &ins, &ins_size));
        cbt->ins_head = ins_head;
        cbt->ins = ins;

        if (upd_arg == NULL) {
            WT_ERR(__wt_upd_alloc(session, value, modify_type, &upd, &upd_size));
            WT_ERR(__wt_txn_modify(session, upd));
            added_to_txn = true;

            /* Avoid a data copy in WT_CURSOR.update. */
            __wt_upd_value_assign(cbt->modify_update, upd);
        } else {
            /*
             * If this refers to the delta-enabled case, we can skip the following checks. We either
             * insert a tombstone with a standard update or only a standard update to the history
             * store if we write a prepared update to the data store.
             */
            WT_ASSERT(session,
              WT_DELTA_LEAF_ENABLED(session) || !WT_IS_HS(S2BT(session)->dhandle) ||
                (upd_arg->type == WT_UPDATE_TOMBSTONE && upd_arg->next != NULL &&
                  upd_arg->next->type == WT_UPDATE_STANDARD && upd_arg->next->next == NULL) ||
                (upd_arg->type == WT_UPDATE_STANDARD && upd_arg->next == NULL));

            upd_size = __wt_update_list_memsize(upd);
        }

        ins->upd = upd;
        ins_size += upd_size;

        /*
         * If there was no insert list during the search, the cursor's information cannot be
         * correct, search couldn't have initialized it.
         *
         * Otherwise, point the new WT_INSERT item's skiplist to the next elements in the insert
         * list (which we will check are still valid inside the serialization function).
         *
         * The serial mutex acts as our memory barrier to flush these writes before inserting them
         * into the list.
         */
        if (cbt->ins_stack[0] == NULL)
            for (i = 0; i < skipdepth; i++) {
                cbt->ins_stack[i] = &ins_head->head[i];
                ins->next[i] = cbt->next_stack[i] = NULL;
            }
        else
            for (i = 0; i < skipdepth; i++)
                ins->next[i] = cbt->next_stack[i];

        /* Insert the WT_INSERT structure. */
        WT_ERR(__wt_insert_serial(
          session, page, cbt->ins_head, cbt->ins_stack, &ins, ins_size, skipdepth, exclusive));
    }

    inserted_to_update_chain = true;

    /*
     * If the update was successful, add it to the in-memory log.
     *
     * We will enter this code if we are doing cursor operations (upd_arg == NULL). We may fail
     * here. However, we have already successfully inserted the updates to the update chain. In this
     * case, don't free the allocated memory in error handling. Leave them to the rollback logic to
     * do the cleanup.
     *
     * If we are calling for internal purposes (upd_arg != NULL), we skip this code. Therefore, we
     * cannot fail after we have inserted the updates to the update chain. The caller of this
     * function can safely free the updates if it receives an error return.
     */
    if (added_to_txn && modify_type != WT_UPDATE_RESERVE) {
        if (__wt_txn_log_op_check(session))
            WT_ERR(__wt_txn_log_op(session, cbt));

        /*
         * Set the key in the transaction operation to be used in case this transaction is prepared
         * to retrieve the update corresponding to this operation.
         */
        WT_ERR(__wt_txn_op_set_key(session, key));
    }

    if (0) {
err:
        /*
         * Let the rollback logic to do the cleanup if we have inserted the update to the update
         * chain.
         */
        if (!inserted_to_update_chain) {
            /*
             * Remove the update from the current transaction, don't try to modify it on rollback.
             */
            if (added_to_txn)
                __wt_txn_unmodify(session);

            /* Free any allocated insert list object. */
            __wt_free(session, ins);

            cbt->ins = NULL;

            /* Discard any allocated update, unless we failed after linking it into page memory. */
            if (upd_arg == NULL)
                __wt_free(session, upd);

            /*
             * When prepending a list of updates to an update chain, we link them together; sever
             * that link so our callers list doesn't point into page memory.
             */
            if (last_upd != NULL)
                last_upd->next = NULL;
        }

        /*
         * If upd was freed or if we know that its ownership was moved to a page, set the update
         * argument to NULL to prevent future use by the caller.
         */
        if (upd == NULL && updp_arg != NULL)
            *updp_arg = NULL;
    }

    return (ret);
}

/*
 * __wt_update_obsolete_check --
 *     Check for obsolete updates and force evict the page if the update list is too long.
 */
void
__wt_update_obsolete_check(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_UPDATE *upd)
{
    WT_PAGE *page;
    WT_TXN_GLOBAL *txn_global;
    WT_UPDATE *first;
    wt_timestamp_t prune_timestamp;
    uint64_t oldest_id;
    u_int count;

    page = cbt->ref->page;
    txn_global = &S2C(session)->txn_global;

    WT_ASSERT(session, page->modify != NULL);
    /* If we can't lock it, don't scan, that's okay. */
    if (WT_PAGE_TRYLOCK(session, page) != 0)
        return;

    WT_ACQUIRE_READ(prune_timestamp, CUR2BT(cbt)->prune_timestamp);

    oldest_id = __wt_txn_oldest_id(session);

    /*
     * This function identifies obsolete updates, and truncates them from the rest of the chain;
     * because this routine is called from inside a serialization function, the caller has
     * responsibility for actually freeing the memory.
     *
     * Walk the list of updates, looking for obsolete updates at the end.
     *
     * Only updates with globally visible, self-contained data can terminate update chains.
     */
    for (first = NULL, count = 0; upd != NULL; upd = upd->next, count++) {
        if (upd->txnid == WT_TXN_ABORTED)
            continue;

        /*
         * Prepare transaction rollback adds a globally visible tombstone to the update chain to
         * remove the entire key. Treating these globally visible tombstones as obsolete and
         * trimming update list can cause problems if the update chain is getting accessed somewhere
         * else. To avoid this problem, skip these globally visible tombstones from the update
         * obsolete check.
         */
        if (F_ISSET(upd, WT_UPDATE_PREPARE_ROLLBACK))
            continue;

        /*
         * If a table has garbage collection enabled, then trim updates as possible. We should check
         * the logic here - it might be possible to do something more aggressive?
         */
        if (__wt_txn_upd_visible_all(session, upd) ||
          (F_ISSET(CUR2BT(cbt), WT_BTREE_GARBAGE_COLLECT) &&
            (upd->txnid < oldest_id && prune_timestamp != WT_TS_NONE &&
              upd->upd_durable_ts <= prune_timestamp))) {
            if (first == NULL && WT_UPDATE_DATA_VALUE(upd))
                first = upd;
        } else
            first = NULL;

        /* Cannot truncate the updates if we need to remove the updates from the history store. */
        if (F_ISSET(upd, WT_UPDATE_HS_MAX_STOP))
            first = NULL;
    }

    if (first != NULL && first->next != NULL)
        __wt_free_obsolete_updates(session, page, first);

    /*
     * Force evict a page when there are more than WT_THOUSAND updates to a single item. Increasing
     * the minSnapshotHistoryWindowInSeconds to 300 introduced a performance regression in which the
     * average number of updates on a single item was approximately 1000 in write-heavy workloads.
     * This is why we use WT_THOUSAND here.
     */
    if (count > WT_THOUSAND) {
        WT_STAT_CONN_INCR(session, eviction_force_long_update_list);
        __wt_evict_page_soon(session, cbt->ref);
    }

    if (first == NULL) {
        /*
         * If the list is long, don't retry checks on this page until the transaction state has
         * moved forwards.
         */
        if (count > 20) {
            page->modify->obsolete_check_txn = __wt_atomic_loadv64(&txn_global->last_running);
            if (txn_global->has_pinned_timestamp)
                page->modify->obsolete_check_timestamp = txn_global->pinned_timestamp;
        }
    }

    WT_PAGE_UNLOCK(session, page);
}
