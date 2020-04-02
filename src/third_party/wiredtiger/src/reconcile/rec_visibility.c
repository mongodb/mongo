/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __rec_update_stable --
 *     Return whether an update is stable or not.
 */
static bool
__rec_update_stable(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_UPDATE *upd)
{
    return (F_ISSET(r, WT_REC_VISIBLE_ALL) ?
        __wt_txn_upd_visible_all(session, upd) :
        __wt_txn_upd_visible_type(session, upd) == WT_VISIBLE_TRUE &&
          __wt_txn_visible(session, upd->txnid, upd->start_ts));
}

/*
 * __rec_update_save --
 *     Save a WT_UPDATE list for later restoration.
 */
static int
__rec_update_save(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_INSERT *ins, void *ripcip,
  WT_UPDATE *onpage_upd, size_t upd_memsize)
{
    WT_SAVE_UPD *supd;

    WT_RET(__wt_realloc_def(session, &r->supd_allocated, r->supd_next + 1, &r->supd));
    supd = &r->supd[r->supd_next];
    supd->ins = ins;
    supd->ripcip = ripcip;
    WT_CLEAR(supd->onpage_upd);
    if (onpage_upd != NULL &&
      (onpage_upd->type == WT_UPDATE_STANDARD || onpage_upd->type == WT_UPDATE_MODIFY))
        supd->onpage_upd = onpage_upd;
    ++r->supd_next;
    r->supd_memsize += upd_memsize;
    return (0);
}

/*
 * __rec_append_orig_value --
 *     Append the key's original value to its update list.
 */
static int
__rec_append_orig_value(
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_UPDATE *upd, WT_CELL_UNPACK *unpack)
{
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_UPDATE *append, *tombstone;
    size_t size, total_size;

    for (;; upd = upd->next) {
        /* Done if at least one self-contained update is globally visible. */
        if (WT_UPDATE_DATA_VALUE(upd) && __wt_txn_upd_visible_all(session, upd))
            return (0);

        /*
         * If the update is restored from the history store for the rollback to stable operation we
         * don't need the on-disk value anymore and we're done.
         */
        if (F_ISSET(upd, WT_UPDATE_RESTORED_FOR_ROLLBACK))
            return (0);

        /* On page value already on chain */
        if (unpack != NULL && unpack->start_ts == upd->start_ts && unpack->start_txn == upd->txnid)
            return (0);

        /* Leave reference at the last item in the chain. */
        if (upd->next == NULL)
            break;
    }

    /*
     * We need the original on-page value for some reader: get a copy and append it to the end of
     * the update list with a transaction ID that guarantees its visibility.
     *
     * If we don't have a value cell, it's an insert/append list key/value pair which simply doesn't
     * exist for some reader; place a deleted record at the end of the update list.
     *
     * If the an update is out of order so it masks the value in the cell, don't append.
     */
    append = tombstone = NULL; /* -Wconditional-uninitialized */
    total_size = size = 0;     /* -Wconditional-uninitialized */
    if (unpack == NULL || unpack->type == WT_CELL_DEL)
        WT_RET(__wt_update_alloc(session, NULL, &append, &size, WT_UPDATE_TOMBSTONE));
    else {
        WT_RET(__wt_scr_alloc(session, 0, &tmp));
        WT_ERR(__wt_page_cell_data_ref(session, page, unpack, tmp));
        WT_ERR(__wt_update_alloc(session, tmp, &append, &size, WT_UPDATE_STANDARD));
        append->start_ts = append->durable_ts = unpack->start_ts;
        append->txnid = unpack->start_txn;
        total_size = size;

        /*
         * We need to append a TOMBSTONE before the onpage value if the onpage value has a valid
         * stop pair.
         *
         * Imagine a case we insert and delete a value respectively at timestamp 0 and 10, and later
         * insert it again at 20. We need the TOMBSTONE to tell us there is no value between 10 and
         * 20.
         */
        if (unpack->stop_ts != WT_TS_MAX || unpack->stop_txn != WT_TXN_MAX) {
            WT_ERR(__wt_update_alloc(session, NULL, &tombstone, &size, WT_UPDATE_TOMBSTONE));
            tombstone->txnid = unpack->stop_txn;
            tombstone->start_ts = unpack->stop_ts;
            tombstone->durable_ts = unpack->stop_ts;
            tombstone->next = append;
            total_size += size;
        }
    }

    if (tombstone != NULL)
        append = tombstone;

    /* Append the new entry into the update list. */
    WT_PUBLISH(upd->next, append);

    __wt_cache_page_inmem_incr(session, page, total_size);

err:
    __wt_scr_free(session, &tmp);
    /* Free append when tombstone allocation fails */
    if (ret != 0) {
        __wt_free_update_list(session, &append);
    }
    return (ret);
}

/*
 * __rec_need_save_upd --
 *     Return if we need to save the update chain
 */
static bool
__rec_need_save_upd(
  WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_UPDATE_SELECT *upd_select, bool has_newer_updates)
{
    if (F_ISSET(r, WT_REC_EVICT) && has_newer_updates)
        return (true);

    /*
     * Save updates for any reconciliation that doesn't involve history store (in-memory database
     * and fixed length column store), except when the maximum timestamp and txnid are globally
     * visible.
     */
    if (!F_ISSET(r, WT_REC_HS) && !F_ISSET(r, WT_REC_IN_MEMORY) && r->page->type != WT_PAGE_COL_FIX)
        return (false);

    /* When in checkpoint, no need to save update if no onpage value is selected. */
    if (F_ISSET(r, WT_REC_CHECKPOINT) && upd_select->upd == NULL)
        return (false);

    return (!__wt_txn_visible_all(session, upd_select->stop_txn, upd_select->stop_ts) &&
      !__wt_txn_visible_all(session, upd_select->start_txn, upd_select->start_ts));
}

/*
 * __wt_rec_upd_select --
 *     Return the update in a list that should be written (or NULL if none can be written).
 */
int
__wt_rec_upd_select(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_INSERT *ins, void *ripcip,
  WT_CELL_UNPACK *vpack, WT_UPDATE_SELECT *upd_select)
{
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_PAGE *page;
    WT_UPDATE *first_txn_upd, *first_upd, *upd, *last_upd;
    wt_timestamp_t checkpoint_timestamp, max_ts, tombstone_durable_ts;
    size_t size, upd_memsize;
    uint64_t max_txn, txnid;
    bool has_newer_updates, is_hs_page, upd_saved;

    /*
     * The "saved updates" return value is used independently of returning an update we can write,
     * both must be initialized.
     */
    upd_select->upd = NULL;
    upd_select->start_durable_ts = WT_TS_NONE;
    upd_select->start_ts = WT_TS_NONE;
    upd_select->start_txn = WT_TXN_NONE;
    upd_select->stop_durable_ts = WT_TS_NONE;
    upd_select->stop_ts = WT_TS_MAX;
    upd_select->stop_txn = WT_TXN_MAX;

    page = r->page;
    first_txn_upd = upd = last_upd = NULL;
    upd_memsize = 0;
    checkpoint_timestamp = S2C(session)->txn_global.checkpoint_timestamp;
    max_ts = WT_TS_NONE;
    tombstone_durable_ts = WT_TS_NONE;
    max_txn = WT_TXN_NONE;
    has_newer_updates = upd_saved = false;
    is_hs_page = F_ISSET(S2BT(session), WT_BTREE_HS);

    /*
     * If called with a WT_INSERT item, use its WT_UPDATE list (which must exist), otherwise check
     * for an on-page row-store WT_UPDATE list (which may not exist). Return immediately if the item
     * has no updates.
     */
    if (ins != NULL)
        first_upd = ins->upd;
    else if ((first_upd = WT_ROW_UPDATE(page, ripcip)) == NULL)
        return (0);

    for (upd = first_upd; upd != NULL; upd = upd->next) {
        if ((txnid = upd->txnid) == WT_TXN_ABORTED)
            continue;

        ++r->updates_seen;
        upd_memsize += WT_UPDATE_MEMSIZE(upd);

        /*
         * Track the first update in the chain that is not aborted and the maximum transaction ID.
         */
        if (first_txn_upd == NULL)
            first_txn_upd = upd;
        if (WT_TXNID_LT(max_txn, txnid))
            max_txn = txnid;

        /*
         * Check whether the update was committed before reconciliation started. The global commit
         * point can move forward during reconciliation so we use a cached copy to avoid races when
         * a concurrent transaction commits or rolls back while we are examining its updates. This
         * check is not required for history store updates as they are implicitly committed. As
         * prepared transaction IDs are globally visible, need to check the update state as well.
         */
        if (!is_hs_page && (F_ISSET(r, WT_REC_VISIBLE_ALL) ? WT_TXNID_LE(r->last_running, txnid) :
                                                             !__txn_visible_id(session, txnid))) {
            has_newer_updates = true;
            continue;
        }
        if (upd->prepare_state == WT_PREPARE_LOCKED ||
          upd->prepare_state == WT_PREPARE_INPROGRESS) {
            has_newer_updates = true;
            if (upd->start_ts > max_ts)
                max_ts = upd->start_ts;

            /*
             * Track the oldest update not on the page, used to decide whether reads can use the
             * page image, hence using the start rather than the durable timestamp.
             */
            if (upd->start_ts < r->min_skipped_ts)
                r->min_skipped_ts = upd->start_ts;
            continue;
        }

        /* Track the first update with non-zero timestamp. */
        if (upd->start_ts > max_ts)
            max_ts = upd->start_ts;

        /*
         * FIXME-prepare-support: A temporary solution for not storing durable timestamp in the
         * cell. Properly fix this problem in PM-1524. It is currently not OK to write prepared
         * updates with durable timestamp larger than checkpoint timestamp to data store as we don't
         * store durable timestamp in the cell. However, it is OK to write them to the history store
         * as we store the durable timestamp in the history store value.
         */
        if (upd->durable_ts != upd->start_ts && upd->durable_ts > checkpoint_timestamp) {
            has_newer_updates = true;
            continue;
        }

        /* Always select the newest committed update to write to disk */
        if (upd_select->upd == NULL)
            upd_select->upd = upd;

        if (!__rec_update_stable(session, r, upd)) {
            if (F_ISSET(r, WT_REC_EVICT))
                ++r->updates_unstable;

            /*
             * Rare case: when applications run at low isolation levels, update/restore eviction may
             * see a stable update followed by an uncommitted update. Give up in that case: we need
             * to discard updates from the stable update and older for correctness and we can't
             * discard an uncommitted update.
             */
            if (upd_select->upd != NULL && has_newer_updates)
                return (__wt_set_return(session, EBUSY));
        } else if (!F_ISSET(r, WT_REC_EVICT))
            break;
    }

    /* Keep track of the selected update. */
    upd = upd_select->upd;

    /* Reconciliation should never see an aborted or reserved update. */
    WT_ASSERT(
      session, upd == NULL || (upd->txnid != WT_TXN_ABORTED && upd->type != WT_UPDATE_RESERVE));

    /*
     * The checkpoint transaction is special. Make sure we never write metadata updates from a
     * checkpoint in a concurrent session.
     */
    WT_ASSERT(session, !WT_IS_METADATA(session->dhandle) || upd == NULL ||
        upd->txnid == WT_TXN_NONE || upd->txnid != S2C(session)->txn_global.checkpoint_state.id ||
        WT_SESSION_IS_CHECKPOINT(session));

    /* If all of the updates were aborted, quit. */
    if (first_txn_upd == NULL) {
        WT_ASSERT(session, upd == NULL);
        return (0);
    }

    /*
     * We expect the page to be clean after reconciliation. If there are invisible updates, abort
     * eviction.
     */
    if (has_newer_updates && F_ISSET(r, WT_REC_CLEAN_AFTER_REC | WT_REC_VISIBILITY_ERR)) {
        if (F_ISSET(r, WT_REC_VISIBILITY_ERR))
            WT_PANIC_RET(session, EINVAL, "reconciliation error, update not visible");
        return (__wt_set_return(session, EBUSY));
    }

    if (upd != NULL && upd->start_ts > r->max_ondisk_ts)
        r->max_ondisk_ts = upd->start_ts;

    /*
     * The start timestamp is determined by the commit timestamp when the key is first inserted (or
     * last updated). The end timestamp is set when a key/value pair becomes invalid, either because
     * of a remove or a modify/update operation on the same key.
     *
     * In the case of a tombstone where the previous update is the ondisk value, we'll allocate an
     * update here to represent the ondisk value. Keep a pointer to the original update (the
     * tombstone) since we do some pointer comparisons below to check whether or not all updates are
     * stable.
     */
    if (upd != NULL) {
        /*
         * If the newest is a tombstone then select the update before it and set the end of the
         * visibility window to its time pair as appropriate to indicate that we should return "not
         * found" for reads after this point.
         *
         * Otherwise, leave the end of the visibility window at the maximum possible value to
         * indicate that the value is visible to any timestamp/transaction id ahead of it.
         */
        if (upd->type == WT_UPDATE_TOMBSTONE) {
            upd_select->stop_ts = upd->start_ts;
            if (upd->txnid != WT_TXN_NONE)
                upd_select->stop_txn = upd->txnid;
            if (upd->durable_ts != WT_TS_NONE)
                tombstone_durable_ts = upd->durable_ts;

            /* Find the update this tombstone applies to. */
            if (!__wt_txn_visible_all(session, upd->txnid, upd->start_ts)) {
                while (upd->next != NULL && upd->next->txnid == WT_TXN_ABORTED)
                    upd = upd->next;
                WT_ASSERT(session, upd->next == NULL || upd->next->txnid != WT_TXN_ABORTED);
                if (upd->next == NULL)
                    last_upd = upd;
                upd_select->upd = upd = upd->next;
            }
        }
        if (upd != NULL) {
            /* The beginning of the validity window is the selected update's time pair. */
            upd_select->start_durable_ts = upd_select->start_ts = upd->start_ts;
            /* If durable timestamp is provided, use it. */
            if (upd->durable_ts != WT_TS_NONE)
                upd_select->start_durable_ts = upd->durable_ts;
            upd_select->start_txn = upd->txnid;

            /* Use the tombstone durable timestamp as the overall durable timestamp if it exists. */
            if (tombstone_durable_ts != WT_TS_NONE)
                upd_select->stop_durable_ts = tombstone_durable_ts;
        } else if (upd_select->stop_ts != WT_TS_NONE || upd_select->stop_txn != WT_TXN_NONE) {
            /* If we only have a tombstone in the update list, we must have an ondisk value. */
            WT_ASSERT(session, vpack != NULL);
            /*
             * It's possible to have a tombstone as the only update in the update list. If we
             * reconciled before with only a single update and then read the page back into cache,
             * we'll have an empty update list. And applying a delete on top of that will result in
             * ONLY a tombstone in the update list.
             *
             * In this case, we should leave the selected update unset to indicate that we want to
             * keep the same on-disk value but set the stop time pair to indicate that the validity
             * window ends when this tombstone started.
             */
            upd_select->start_durable_ts = upd_select->start_ts = vpack->start_ts;
            upd_select->start_txn = vpack->start_txn;

            /* Use the tombstone durable timestamp as the overall durable timestamp if it exists. */
            if (tombstone_durable_ts != WT_TS_NONE)
                upd_select->stop_durable_ts = tombstone_durable_ts;

            /*
             * Leaving the update unset means that we can skip reconciling. If we've set the stop
             * time pair because of a tombstone after the on-disk value, we still have work to do so
             * that is NOT ok. Let's append the on-disk value to the chain.
             */
            WT_ERR(__wt_scr_alloc(session, 0, &tmp));
            WT_ERR(__wt_page_cell_data_ref(session, page, vpack, tmp));
            WT_ERR(__wt_update_alloc(session, tmp, &upd, &size, WT_UPDATE_STANDARD));
            upd->start_ts = upd->durable_ts = vpack->start_ts;
            upd->txnid = vpack->start_txn;
            WT_PUBLISH(last_upd->next, upd);
            /* This is going in our update list so it should be accounted for in cache usage. */
            __wt_cache_page_inmem_incr(session, page, size);
            upd_select->upd = upd;
        }
    }
    /*
     * If we've set the stop to a zeroed pair, we intend to remove the key. Instead of selecting the
     * onpage value and setting the stop a zeroed time pair which would trigger a rewrite of the
     * cell with the new stop time pair, we should unset the selected update so the key itself gets
     * omitted from the new page image.
     */
    if (upd_select->stop_ts == WT_TS_NONE && upd_select->stop_txn == WT_TXN_NONE)
        upd_select->upd = NULL;

    /*
     * If we found a tombstone with a time pair earlier than the update it applies to, which can
     * happen if the application performs operations with timestamps out-of-order, make it invisible
     * by making the start time pair match the stop time pair of the tombstone. We don't guarantee
     * that older readers will be able to continue reading content that has been made invisible by
     * out-of-order updates.
     *
     * Note that we carefully don't take this path when the stop time pair is equal to the start
     * time pair. While unusual, it is permitted for a single transaction to insert and then remove
     * a record. We don't want to generate a warning in that case.
     */
    if (upd_select->stop_ts < upd_select->start_ts ||
      (upd_select->stop_ts == upd_select->start_ts &&
          upd_select->stop_txn < upd_select->start_txn)) {
        char ts_string[2][WT_TS_INT_STRING_SIZE];
        __wt_verbose(session, WT_VERB_TIMESTAMP,
          "Warning: fixing out-of-order timestamps remove at %s earlier than value at %s",
          __wt_timestamp_to_string(upd_select->stop_ts, ts_string[0]),
          __wt_timestamp_to_string(upd_select->start_ts, ts_string[1]));
        upd_select->start_durable_ts = upd_select->stop_durable_ts;
        upd_select->start_ts = upd_select->stop_ts;
        upd_select->start_txn = upd_select->stop_txn;
    }

    /*
     * Track the most recent transaction in the page. We store this in the tree at the end of
     * reconciliation in the service of checkpoints, it is used to avoid discarding trees from
     * memory when they have changes required to satisfy a snapshot read.
     */
    if (WT_TXNID_LT(r->max_txn, max_txn))
        r->max_txn = max_txn;

    /* Update the maximum timestamp. */
    if (max_ts > r->max_ts)
        r->max_ts = max_ts;

    /* Mark the page dirty after reconciliation. */
    if (has_newer_updates)
        r->leave_dirty = true;

    /*
     * We should restore the update chains to the new disk image if there are newer updates in
     * eviction.
     */
    if (has_newer_updates && F_ISSET(r, WT_REC_EVICT))
        r->cache_write_restore = true;

    /*
     * The update doesn't have any further updates that need to be written to the history store,
     * skip saving the update as saving the update will cause reconciliation to think there is work
     * that needs to be done when there might not be.
     *
     * Additionally history store reconciliation is not set skip saving an update.
     */
    if (__rec_need_save_upd(session, r, upd_select, has_newer_updates)) {
        WT_ERR(__rec_update_save(session, r, ins, ripcip, upd_select->upd, upd_memsize));
        upd_saved = true;
    }

    /*
     * Paranoia: check that we didn't choose an update that has since been rolled back.
     */
    WT_ASSERT(session, upd_select->upd == NULL || upd_select->upd->txnid != WT_TXN_ABORTED);

    /*
     * Returning an update means the original on-page value might be lost, and that's a problem if
     * there's a reader that needs it. This call makes a copy of the on-page value. We do that any
     * time there are saved updates and during reconciliation of a backing overflow record that will
     * be physically removed once it's no longer needed.
     */
    if (upd_select->upd != NULL &&
      (upd_saved || (vpack != NULL && F_ISSET(vpack, WT_CELL_UNPACK_OVERFLOW) &&
                      vpack->raw != WT_CELL_VALUE_OVFL_RM)))
        WT_ERR(__rec_append_orig_value(session, page, upd_select->upd, vpack));

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}
