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
static inline bool
__rec_update_stable(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_UPDATE *upd)
{
    return (F_ISSET(r, WT_REC_VISIBLE_ALL) ? __wt_txn_upd_visible_all(session, upd) :
                                             __wt_txn_upd_visible(session, upd) &&
          __wt_txn_visible(session, upd->txnid, upd->durable_ts));
}

/*
 * __rec_update_save --
 *     Save a WT_UPDATE list for later restoration.
 */
static inline int
__rec_update_save(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_INSERT *ins, void *ripcip,
  WT_UPDATE *onpage_upd, bool supd_restore, size_t upd_memsize)
{
    WT_SAVE_UPD *supd;

    /* If nothing is committed, we must restore the update chain. */
    WT_ASSERT(session, onpage_upd != NULL || supd_restore);
    /* We can only write a standard update or a modify to the data store. */
    WT_ASSERT(session, onpage_upd == NULL || onpage_upd->type == WT_UPDATE_STANDARD ||
        onpage_upd->type == WT_UPDATE_MODIFY);

    WT_RET(__wt_realloc_def(session, &r->supd_allocated, r->supd_next + 1, &r->supd));
    supd = &r->supd[r->supd_next];
    supd->ins = ins;
    supd->ripcip = ripcip;
    supd->onpage_upd = onpage_upd;
    supd->restore = supd_restore;
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
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_UPDATE *upd, WT_CELL_UNPACK_KV *unpack)
{
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_UPDATE *append, *oldest_upd, *tombstone;
    size_t size, total_size;
    bool tombstone_globally_visible;

    WT_ASSERT(session, upd != NULL && unpack != NULL && unpack->type != WT_CELL_DEL);

    append = oldest_upd = tombstone = NULL;
    total_size = 0;
    tombstone_globally_visible = false;

    /* Review the current update list, checking conditions that mean no work is needed. */
    for (;; upd = upd->next) {
        /* Done if the update was restored from the history store. */
        if (F_ISSET(upd, WT_UPDATE_RESTORED_FROM_HS))
            return (0);

        /*
         * Prepared updates should already be in the update list, add the original update to the
         * list only when the prepared update is a tombstone.
         */
        if (unpack->tw.prepare && upd->type != WT_UPDATE_TOMBSTONE)
            return (0);

        /*
         * Done if the on page value already appears on the update list. We can't do the same check
         * for stop time point because we may still need to append the onpage value if only the
         * tombstone is on the update chain.
         */
        if (unpack->tw.start_ts == upd->start_ts && unpack->tw.start_txn == upd->txnid &&
          upd->type != WT_UPDATE_TOMBSTONE)
            return (0);

        /*
         * Done if at least one self-contained update is globally visible. It's tempting to pull
         * this test out of the loop and only test the oldest self-contained update for global
         * visibility (as visibility tests are expensive). However, when running at lower isolation
         * levels, or when an application intentionally commits in out of timestamp order, it's
         * possible for an update on the chain to be globally visible and followed by an (earlier)
         * update that is not yet globally visible.
         */
        if (WT_UPDATE_DATA_VALUE(upd) && __wt_txn_upd_visible_all(session, upd))
            return (0);

        if (upd->txnid != WT_TXN_ABORTED)
            oldest_upd = upd;

        /* Leave reference pointing to the last item in the update list. */
        if (upd->next == NULL)
            break;
    }

    /*
     * We end up in this function because we have selected a newer value to write to disk. If we
     * select the newest committed update, we should see a valid update here. We can only write
     * uncommitted prepared updates in eviction and if the update chain only has uncommitted
     * prepared updates, we cannot abort them concurrently when we are still evicting the page
     * because we have to do a search for the prepared updates, which can not proceed until eviction
     * finishes.
     */
    WT_ASSERT(session, oldest_upd != NULL);

    /*
     * Additionally, we need to append a tombstone before the onpage value we're about to append to
     * the list, if the onpage value has a valid stop time point. Imagine a case where we insert and
     * delete a value respectively at timestamp 0 and 10, and later insert it again at 20. We need
     * the tombstone to tell us there is no value between 10 and 20.
     */
    if (WT_TIME_WINDOW_HAS_STOP(&unpack->tw)) {
        tombstone_globally_visible = __wt_txn_tw_stop_visible_all(session, &unpack->tw);

        /*
         * No need to append the tombstone if it is already in the update chain.
         *
         * Don't append the onpage tombstone if it is a prepared update as it is either on the
         * update chain or has been aborted. If it is aborted, discard it silently.
         */
        if (oldest_upd->type != WT_UPDATE_TOMBSTONE && !unpack->tw.prepare) {
            /*
             * We still need to append the globally visible tombstone if its timestamp is WT_TS_NONE
             * as we may need it to clear the history store content of the key. We don't append a
             * timestamped globally visible tombstone because even if its timestamp is smaller than
             * the entries in the history store, we can't change the history store entries. This is
             * not correct but we hope we can get away with it.
             */
            if (unpack->tw.durable_stop_ts != WT_TS_NONE && tombstone_globally_visible)
                return (0);

            WT_ERR(__wt_upd_alloc_tombstone(session, &tombstone, &size));
            total_size += size;
            tombstone->txnid = unpack->tw.stop_txn;
            tombstone->start_ts = unpack->tw.stop_ts;
            tombstone->durable_ts = unpack->tw.durable_stop_ts;
            F_SET(tombstone, WT_UPDATE_RESTORED_FROM_DS);
        } else {
            /*
             * Once the prepared update is resolved, the in-memory update and on-disk written copy
             * doesn't have same timestamp due to replacing of prepare timestamp with commit and
             * durable timestamps. Don't compare them when the on-disk version is a prepare.
             */
            WT_ASSERT(session, unpack->tw.prepare || (unpack->tw.stop_ts == oldest_upd->start_ts &&
                                                       unpack->tw.stop_txn == oldest_upd->txnid));
            if (tombstone_globally_visible)
                return (0);
        }
    } else if (unpack->tw.prepare)
        /*
         * Don't append the onpage value if it is a prepared update as it is either on the update
         * chain or has been aborted. If it is aborted, discard it silently.
         */
        return (0);

    /* We need the original on-page value for some reader: get a copy. */
    if (!tombstone_globally_visible) {
        WT_ERR(__wt_scr_alloc(session, 0, &tmp));
        WT_ERR(__wt_page_cell_data_ref(session, page, unpack, tmp));
        WT_ERR(__wt_upd_alloc(session, tmp, WT_UPDATE_STANDARD, &append, &size));
        total_size += size;
        append->txnid = unpack->tw.start_txn;
        append->start_ts = unpack->tw.start_ts;
        append->durable_ts = unpack->tw.durable_start_ts;
        F_SET(append, WT_UPDATE_RESTORED_FROM_DS);
    }

    if (tombstone != NULL) {
        tombstone->next = append;
        append = tombstone;
    }

    /* Append the new entry into the update list. */
    WT_PUBLISH(upd->next, append);

    __wt_cache_page_inmem_incr(session, page, total_size);

    if (0) {
err:
        __wt_free(session, append);
        __wt_free(session, tombstone);
    }
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __rec_need_save_upd --
 *     Return if we need to save the update chain
 */
static inline bool
__rec_need_save_upd(
  WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_UPDATE_SELECT *upd_select, bool has_newer_updates)
{
    if (upd_select->tw.prepare)
        return (true);

    if (F_ISSET(r, WT_REC_EVICT) && has_newer_updates)
        return (true);

    /*
     * Save updates for any reconciliation that doesn't involve history store (in-memory database
     * and fixed length column store), except when the selected stop time point or the selected
     * start time point is globally visible.
     */
    if (!F_ISSET(r, WT_REC_HS) && !F_ISSET(r, WT_REC_IN_MEMORY) && r->page->type != WT_PAGE_COL_FIX)
        return (false);

    /* When in checkpoint, no need to save update if no onpage value is selected. */
    if (F_ISSET(r, WT_REC_CHECKPOINT) && upd_select->upd == NULL)
        return (false);

    return (!__wt_txn_tw_stop_visible_all(session, &upd_select->tw) &&
      !__wt_txn_tw_start_visible_all(session, &upd_select->tw));
}

/*
 * __wt_rec_upd_select --
 *     Return the update in a list that should be written (or NULL if none can be written).
 */
int
__wt_rec_upd_select(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_INSERT *ins, void *ripcip,
  WT_CELL_UNPACK_KV *vpack, WT_UPDATE_SELECT *upd_select)
{
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_PAGE *page;
    WT_TIME_WINDOW *select_tw;
    WT_UPDATE *first_txn_upd, *first_upd, *upd, *last_upd, *tombstone;
    wt_timestamp_t max_ts;
    size_t upd_memsize;
    uint64_t max_txn, txnid;
    char time_string[WT_TIME_STRING_SIZE];
    bool has_newer_updates, is_hs_page, supd_restore, upd_saved;

    /*
     * The "saved updates" return value is used independently of returning an update we can write,
     * both must be initialized.
     */
    upd_select->upd = NULL;
    upd_select->upd_saved = false;
    select_tw = &upd_select->tw;
    WT_TIME_WINDOW_INIT(select_tw);

    page = r->page;
    first_txn_upd = upd = last_upd = tombstone = NULL;
    upd_memsize = 0;
    max_ts = WT_TS_NONE;
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
            /*
             * Rare case: when applications run at low isolation levels, eviction may see a
             * committed update followed by uncommitted updates. Give up in that case because we
             * can't move uncommitted updates to the history store.
             */
            if (upd_select->upd != NULL)
                return (__wt_set_return(session, EBUSY));

            has_newer_updates = true;
            continue;
        }

        /* Ignore prepared updates if it is checkpoint. */
        if (upd->prepare_state == WT_PREPARE_LOCKED ||
          upd->prepare_state == WT_PREPARE_INPROGRESS) {
            WT_ASSERT(session, upd_select->upd == NULL || upd_select->upd->txnid == upd->txnid);
            if (F_ISSET(r, WT_REC_CHECKPOINT)) {
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
            } else {
                /*
                 * For prepared updates written to the date store in salvage, we write the same
                 * prepared value to the date store. If there is still content for that key left in
                 * the history store, rollback to stable will bring it back to the data store.
                 * Otherwise, it removes the key.
                 */
                WT_ASSERT(session,
                  F_ISSET(r, WT_REC_EVICT) || (F_ISSET(r, WT_REC_VISIBILITY_ERR) &&
                                                F_ISSET(upd, WT_UPDATE_PREPARE_RESTORED_FROM_DS)));
                WT_ASSERT(session, upd->prepare_state == WT_PREPARE_INPROGRESS);
            }
        }

        /* Track the first update with non-zero timestamp. */
        if (upd->start_ts > max_ts)
            max_ts = upd->start_ts;

        /* Always select the newest committed update to write to disk */
        if (upd_select->upd == NULL)
            upd_select->upd = upd;

        if (F_ISSET(r, WT_REC_EVICT) && !__rec_update_stable(session, r, upd))
            ++r->updates_unstable;
        else if (!F_ISSET(r, WT_REC_EVICT))
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
        upd->txnid == WT_TXN_NONE ||
        upd->txnid != S2C(session)->txn_global.checkpoint_txn_shared.id ||
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
            WT_RET_PANIC(session, EINVAL, "reconciliation error, update not visible");
        return (__wt_set_return(session, EBUSY));
    }

    /* If an update was selected, record that we're making progress. */
    if (upd != NULL)
        r->update_used = true;

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
         * Mark the prepare flag if the selected update is an uncommitted prepare. As tombstone
         * updates are never returned to write, set this flag before we move into the previous
         * update to write.
         */
        if (upd->prepare_state == WT_PREPARE_INPROGRESS)
            select_tw->prepare = 1;

        /*
         * If the newest is a tombstone then select the update before it and set the end of the
         * visibility window to its time point as appropriate to indicate that we should return "not
         * found" for reads after this point.
         *
         * Otherwise, leave the end of the visibility window at the maximum possible value to
         * indicate that the value is visible to any timestamp/transaction id ahead of it.
         */
        if (upd->type == WT_UPDATE_TOMBSTONE) {
            WT_TIME_WINDOW_SET_STOP(select_tw, upd);
            tombstone = upd;

            /* Find the update this tombstone applies to. */
            if (!__wt_txn_upd_visible_all(session, upd)) {
                while (upd->next != NULL && upd->next->txnid == WT_TXN_ABORTED)
                    upd = upd->next;
                WT_ASSERT(session, upd->next == NULL || upd->next->txnid != WT_TXN_ABORTED);
                if (upd->next == NULL)
                    last_upd = upd;
                upd_select->upd = upd = upd->next;
            }
        }
        if (upd != NULL)
            /* The beginning of the validity window is the selected update's time point. */
            WT_TIME_WINDOW_SET_START(select_tw, upd);
        else if (select_tw->stop_ts != WT_TS_NONE || select_tw->stop_txn != WT_TXN_NONE) {
            /* If we only have a tombstone in the update list, we must have an ondisk value. */
            WT_ASSERT(session, vpack != NULL && tombstone != NULL && last_upd->next == NULL);
            /*
             * It's possible to have a tombstone as the only update in the update list. If we
             * reconciled before with only a single update and then read the page back into cache,
             * we'll have an empty update list. And applying a delete on top of that will result in
             * ONLY a tombstone in the update list.
             *
             * In this case, we should leave the selected update unset to indicate that we want to
             * keep the same on-disk value but set the stop time point to indicate that the validity
             * window ends when this tombstone started.
             */
            WT_ERR(__rec_append_orig_value(session, page, tombstone, vpack));

            /*
             * We may have updated the global transaction concurrently and the tombstone is now
             * globally visible. In this case, the on page value is not appended. Check that.
             */
            if (last_upd->next != NULL) {
                WT_ASSERT(session, last_upd->next->txnid == vpack->tw.start_txn &&
                    last_upd->next->start_ts == vpack->tw.start_ts &&
                    last_upd->next->type == WT_UPDATE_STANDARD && last_upd->next->next == NULL);
                upd_select->upd = last_upd->next;
                WT_TIME_WINDOW_SET_START(select_tw, last_upd->next);
            } else {
                /*
                 * If the tombstone is aborted concurrently, we should still have appended the
                 * onpage value.
                 */
                WT_ASSERT(session, tombstone->txnid != WT_TXN_ABORTED &&
                    __wt_txn_upd_visible_all(session, tombstone) && upd_select->upd == NULL);
                upd_select->upd = tombstone;
            }
        }
    }

    /*
     * If we found a tombstone with a time point earlier than the update it applies to, which can
     * happen if the application performs operations with timestamps out-of-order, make it invisible
     * by making the start time point match the stop time point of the tombstone. We don't guarantee
     * that older readers will be able to continue reading content that has been made invisible by
     * out-of-order updates.
     *
     * Note that we carefully don't take this path when the stop time point is equal to the start
     * time point. While unusual, it is permitted for a single transaction to insert and then remove
     * a record. We don't want to generate a warning in that case.
     */
    if (select_tw->stop_ts < select_tw->start_ts ||
      (select_tw->stop_ts == select_tw->start_ts && select_tw->stop_txn < select_tw->start_txn)) {
        __wt_verbose(session, WT_VERB_TIMESTAMP,
          "Warning: fixing out-of-order timestamps remove earlier than value; time window %s",
          __wt_time_window_to_string(select_tw, time_string));

        select_tw->durable_start_ts = select_tw->durable_stop_ts;
        select_tw->start_ts = select_tw->stop_ts;
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
     * The update doesn't have any further updates that need to be written to the history store,
     * skip saving the update as saving the update will cause reconciliation to think there is work
     * that needs to be done when there might not be.
     *
     * Additionally history store reconciliation is not set skip saving an update.
     */
    if (__rec_need_save_upd(session, r, upd_select, has_newer_updates)) {
        /*
         * We should restore the update chains to the new disk image if there are newer updates in
         * eviction, or for cases that don't support history store, such as in-memory database and
         * fixed length column store.
         */
        supd_restore = F_ISSET(r, WT_REC_EVICT) &&
          (has_newer_updates || F_ISSET(S2C(session), WT_CONN_IN_MEMORY) ||
                         page->type == WT_PAGE_COL_FIX);
        if (supd_restore)
            r->cache_write_restore = true;
        WT_ERR(__rec_update_save(session, r, ins, ripcip,
          upd_select->upd != NULL && upd_select->upd->type == WT_UPDATE_TOMBSTONE ? NULL :
                                                                                    upd_select->upd,
          supd_restore, upd_memsize));
        upd_saved = upd_select->upd_saved = true;
    }

    /*
     * Paranoia: check that we didn't choose an update that has since been rolled back.
     */
    WT_ASSERT(session, upd_select->upd == NULL || upd_select->upd->txnid != WT_TXN_ABORTED);

    /*
     * Returning an update means the original on-page value might be lost, and that's a problem if
     * there's a reader that needs it, make a copy of the on-page value. We do that any time there
     * are saved updates (we may need the original on-page value to terminate the update chain, for
     * example, in the case of an update that modifies the original value). Additionally, make a
     * copy of the on-page value if the value is an overflow item and anything other than the
     * on-page cell is being written. This is because the value's backing overflow blocks aren't
     * part of the page, and they are physically removed by checkpoint writing this page, that is,
     * the checkpoint doesn't include the overflow blocks so they're removed and future readers of
     * this page won't be able to find them.
     */
    if (upd_select->upd != NULL && vpack != NULL && vpack->type != WT_CELL_DEL &&
      (upd_saved || F_ISSET(vpack, WT_CELL_UNPACK_OVERFLOW)))
        WT_ERR(__rec_append_orig_value(session, page, upd_select->upd, vpack));

    __wt_time_window_clear_obsolete(
      session, &upd_select->tw, r->rec_start_oldest_id, r->rec_start_pinned_ts);
err:
    __wt_scr_free(session, &tmp);
    return (ret);
}
