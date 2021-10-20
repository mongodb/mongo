/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __rec_update_durable --
 *     Return whether an update is suitable for writing to a disk image.
 */
static bool
__rec_update_durable(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_UPDATE *upd)
{
    return (F_ISSET(r, WT_REC_VISIBLE_ALL) ?
        __wt_txn_upd_visible_all(session, upd) :
        __wt_txn_upd_visible_type(session, upd) == WT_VISIBLE_TRUE &&
          __wt_txn_visible(session, upd->txnid, upd->timestamp));
}

/*
 * __rec_update_save --
 *     Save a WT_UPDATE list for later restoration.
 */
static int
__rec_update_save(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_INSERT *ins, void *ripcip,
  WT_UPDATE *onpage_upd, size_t upd_memsize)
{
    WT_RET(__wt_realloc_def(session, &r->supd_allocated, r->supd_next + 1, &r->supd));
    r->supd[r->supd_next].ins = ins;
    r->supd[r->supd_next].ripcip = ripcip;
    r->supd[r->supd_next].onpage_upd = onpage_upd;
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
    WT_UPDATE *append;
    size_t size;

    /* Done if at least one self-contained update is globally visible. */
    for (;; upd = upd->next) {
        if (WT_UPDATE_DATA_VALUE(upd) && __wt_txn_upd_visible_all(session, upd))
            return (0);

        /* Add the original value after birthmarks. */
        if (upd->type == WT_UPDATE_BIRTHMARK) {
            WT_ASSERT(session, unpack != NULL && unpack->type != WT_CELL_DEL);
            break;
        }

        /* Leave reference at the last item in the chain. */
        if (upd->next == NULL)
            break;
    }

    /*
     * We need the original on-page value for some reader: get a copy and
     * append it to the end of the update list with a transaction ID that
     * guarantees its visibility.
     *
     * If we don't have a value cell, it's an insert/append list key/value
     * pair which simply doesn't exist for some reader; place a deleted
     * record at the end of the update list.
     */
    append = NULL; /* -Wconditional-uninitialized */
    size = 0;      /* -Wconditional-uninitialized */
    if (unpack == NULL || unpack->type == WT_CELL_DEL)
        WT_RET(__wt_update_alloc(session, NULL, &append, &size, WT_UPDATE_TOMBSTONE));
    else {
        WT_RET(__wt_scr_alloc(session, 0, &tmp));
        WT_ERR(__wt_page_cell_data_ref(session, page, unpack, tmp));
        WT_ERR(__wt_update_alloc(session, tmp, &append, &size, WT_UPDATE_STANDARD));
    }

    /*
     * If we're saving the original value for a birthmark, transfer over the transaction ID and
     * clear out the birthmark update. Else, set the entry's transaction information to the lowest
     * possible value (as cleared memory matches the lowest possible transaction ID and timestamp,
     * do nothing).
     */
    if (upd->type == WT_UPDATE_BIRTHMARK) {
        append->txnid = upd->txnid;
        append->timestamp = upd->timestamp;
        append->next = upd->next;
    }

    /* Append the new entry into the update list. */
    WT_PUBLISH(upd->next, append);

    /* Replace the birthmark with an aborted transaction. */
    if (upd->type == WT_UPDATE_BIRTHMARK) {
        WT_ORDERED_WRITE(upd->txnid, WT_TXN_ABORTED);
        WT_ORDERED_WRITE(upd->type, WT_UPDATE_STANDARD);
    }

    __wt_cache_page_inmem_incr(session, page, size);

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __wt_rec_txn_read --
 *     Return the update in a list that should be written (or NULL if none can be written).
 */
int
__wt_rec_txn_read(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_INSERT *ins, void *ripcip,
  WT_CELL_UNPACK *vpack, bool *upd_savedp, WT_UPDATE **updp)
{
    WT_PAGE *page;
    WT_UPDATE *first_txn_upd, *first_upd, *upd;
    wt_timestamp_t max_ts;
    size_t upd_memsize;
    uint64_t max_txn, txnid;
    bool all_stable, list_prepared, list_uncommitted, skipped_birthmark, upd_saved;

    if (upd_savedp != NULL)
        *upd_savedp = false;
    *updp = NULL;

    page = r->page;
    first_txn_upd = NULL;
    upd_memsize = 0;
    max_ts = WT_TS_NONE;
    max_txn = WT_TXN_NONE;
    list_prepared = list_uncommitted = skipped_birthmark = upd_saved = false;

    /*
     * If called with a WT_INSERT item, use its WT_UPDATE list (which must
     * exist), otherwise check for an on-page row-store WT_UPDATE list
     * (which may not exist). Return immediately if the item has no updates.
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
         * a concurrent transaction commits or rolls back while we are examining its updates. As
         * prepared transaction id's are globally visible, need to check the update state as well.
         */
        if (F_ISSET(r, WT_REC_EVICT)) {
            if (F_ISSET(r, WT_REC_VISIBLE_ALL) ? WT_TXNID_LE(r->last_running, txnid) :
                                                 !__txn_visible_id(session, txnid)) {
                r->update_uncommitted = list_uncommitted = true;
                continue;
            }
            if (upd->prepare_state == WT_PREPARE_LOCKED ||
              upd->prepare_state == WT_PREPARE_INPROGRESS) {
                list_prepared = true;
                if (upd->timestamp > max_ts)
                    max_ts = upd->timestamp;

                /*
                 * Track the oldest update not on the page, used to decide whether reads can use the
                 * page image, hence using the start rather than the durable timestamp.
                 */
                if (upd->timestamp < r->min_skipped_ts)
                    r->min_skipped_ts = upd->timestamp;
                continue;
            }
        }

        /* Track the first update with non-zero timestamp. */
        if (upd->timestamp > max_ts)
            max_ts = upd->timestamp;

        /*
         * Find the first update we can use.
         *
         * Update/restore eviction can handle any update (including
         * uncommitted updates).  Lookaside eviction can save any
         * committed update.  Regular eviction checks that the maximum
         * transaction ID and timestamp seen are stable.
         *
         * Lookaside and update/restore eviction try to choose the same
         * version as a subsequent checkpoint, so that checkpoint can
         * skip over pages with lookaside entries.  If the application
         * has supplied a stable timestamp, we assume (a) that it is
         * old, and (b) that the next checkpoint will use it, so we wait
         * to see a stable update.  If there is no stable timestamp, we
         * assume the next checkpoint will write the most recent version
         * (but we save enough information that checkpoint can fix
         * things up if we choose an update that is too new).
         */
        if (*updp == NULL && r->las_skew_newest)
            *updp = upd;

        if (!__rec_update_durable(session, r, upd)) {
            if (F_ISSET(r, WT_REC_EVICT))
                ++r->updates_unstable;

            /*
             * Rare case: when applications run at low isolation levels, update/restore eviction may
             * see a stable update followed by an uncommitted update. Give up in that case: we need
             * to discard updates from the stable update and older for correctness and we can't
             * discard an uncommitted update.
             */
            if (F_ISSET(r, WT_REC_UPDATE_RESTORE) && *updp != NULL &&
              (list_prepared || list_uncommitted))
                return (__wt_set_return(session, EBUSY));

            if (upd->type == WT_UPDATE_BIRTHMARK)
                skipped_birthmark = true;

            /*
             * Track the oldest update not on the page, used to decide whether reads can use the
             * page image, hence using the start rather than the durable timestamp.
             */
            if (*updp == NULL && upd->timestamp < r->min_skipped_ts)
                r->min_skipped_ts = upd->timestamp;

            continue;
        }

        /*
         * Lookaside without stable timestamp was taken care of above
         * (set to the first uncommitted transaction). All other
         * reconciliation takes the first stable update.
         */
        if (*updp == NULL)
            *updp = upd;

        if (!F_ISSET(r, WT_REC_EVICT))
            break;
    }

    /* Keep track of the selected update. */
    upd = *updp;

    /* Reconciliation should never see an aborted or reserved update. */
    WT_ASSERT(
      session, upd == NULL || (upd->txnid != WT_TXN_ABORTED && upd->type != WT_UPDATE_RESERVE));

    /* If all of the updates were aborted, quit. */
    if (first_txn_upd == NULL) {
        WT_ASSERT(session, upd == NULL);
        return (0);
    }

    /* If no updates were skipped, record that we're making progress. */
    if (upd == first_txn_upd)
        r->update_used = true;

    if (upd != NULL && upd->timestamp > r->max_ondisk_ts)
        r->max_ondisk_ts = upd->timestamp;

    /*
     * The checkpoint transaction is special. Make sure we never write metadata updates from a
     * checkpoint in a concurrent session.
     */
    WT_ASSERT(session, !WT_IS_METADATA(session->dhandle) || upd == NULL ||
        upd->txnid == WT_TXN_NONE || upd->txnid != S2C(session)->txn_global.checkpoint_state.id ||
        WT_SESSION_IS_CHECKPOINT(session));

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

    /*
     * If the update we chose was a birthmark, or we are doing update-restore and we skipped a
     * birthmark, the original on-page value must be retained.
     */
    if (upd != NULL && (upd->type == WT_UPDATE_BIRTHMARK ||
                         (F_ISSET(r, WT_REC_UPDATE_RESTORE) && skipped_birthmark))) {
        /*
         * Resolve the birthmark now regardless of whether the update being written to the data file
         * is the same as it was the previous reconciliation. Otherwise lookaside can end up with
         * two birthmark records in the same update chain.
         */
        WT_RET(__rec_append_orig_value(session, page, first_upd, vpack));
        *updp = NULL;
    }

    /*
     * Check if all updates on the page are visible, if not, it must stay dirty.
     *
     * Updates can be out of transaction ID order (but not out of timestamp order), so we track the
     * maximum transaction ID and the newest update with a timestamp (if any).
     */
    all_stable = upd == first_txn_upd && !list_prepared && !list_uncommitted &&
      __wt_txn_visible_all(session, max_txn, max_ts);

    if (all_stable)
        goto check_original_value;

    r->leave_dirty = true;

    if (F_ISSET(r, WT_REC_VISIBILITY_ERR))
        WT_PANIC_RET(session, EINVAL, "reconciliation error, update not visible");

    /* If not trying to evict the page, we know what we'll write and we're done. */
    if (!F_ISSET(r, WT_REC_EVICT))
        goto check_original_value;

    /*
     * We are attempting eviction with changes that are not yet stable
     * (i.e. globally visible).  There are two ways to continue, the
     * save/restore eviction path or the lookaside table eviction path.
     * Both cannot be configured because the paths track different
     * information. The update/restore path can handle uncommitted changes,
     * by evicting most of the page and then creating a new, smaller page
     * to which we re-attach those changes. Lookaside eviction writes
     * changes into the lookaside table and restores them on demand if and
     * when the page is read back into memory.
     *
     * Both paths are configured outside of reconciliation: the save/restore
     * path is the WT_REC_UPDATE_RESTORE flag, the lookaside table path is
     * the WT_REC_LOOKASIDE flag.
     */
    if (!F_ISSET(r, WT_REC_LOOKASIDE | WT_REC_UPDATE_RESTORE))
        return (__wt_set_return(session, EBUSY));
    if (list_uncommitted && !F_ISSET(r, WT_REC_UPDATE_RESTORE))
        return (__wt_set_return(session, EBUSY));

    WT_ASSERT(session, r->max_txn != WT_TXN_NONE);

    /*
     * The order of the updates on the list matters, we can't move only the unresolved updates, move
     * the entire update list.
     */
    WT_RET(__rec_update_save(session, r, ins, ripcip, *updp, upd_memsize));
    upd_saved = true;
    if (upd_savedp != NULL)
        *upd_savedp = true;

check_original_value:
    /*
     * Paranoia: check that we didn't choose an update that has since been rolled back.
     */
    WT_ASSERT(session, *updp == NULL || (*updp)->txnid != WT_TXN_ABORTED);

    /*
     * Returning an update means the original on-page value might be lost, and that's a problem if
     * there's a reader that needs it. This call makes a copy of the on-page value and if there is a
     * birthmark in the update list, replaces it. We do that any time there are saved updates and
     * during reconciliation of a backing overflow record that will be physically removed once it's
     * no longer needed.
     */
    if (*updp != NULL &&
      (upd_saved || (vpack != NULL && vpack->ovfl && vpack->raw != WT_CELL_VALUE_OVFL_RM)))
        WT_RET(__rec_append_orig_value(session, page, first_upd, vpack));

    return (0);
}
