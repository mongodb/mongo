/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __sync_checkpoint_can_skip --
 *     There are limited conditions under which we can skip writing a dirty page during checkpoint.
 */
static WT_INLINE bool
__sync_checkpoint_can_skip(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_MULTI *multi;
    WT_PAGE_MODIFY *mod;
    WT_TXN *txn;
    u_int i;

    WT_ASSERT_SPINLOCK_OWNED(session, &S2BT(session)->flush_lock);

    /*
     * If we got to this point and we are dealing with an internal page, this means at least one of
     * its leaf pages has been reconciled and we need to process the internal page as well.
     */
    if (F_ISSET(ref, WT_REF_FLAG_INTERNAL))
        return (false);

    /*
     * This is the history store btree. As part of the checkpointing the data store, we will move
     * the older values into the history store without using any transactions, we shouldn't ignore
     * them for consistency. Same goes for disaggregated storage metadata.
     */
    if (WT_IS_HS(session->dhandle))
        return (false);
    if (WT_IS_DISAGG_META(session->dhandle))
        return (false);

    /* The checkpoint's snapshot includes the first dirty update on the page. */
    txn = session->txn;
    mod = ref->page->modify;
    if (txn->snapshot_data.snap_max >= mod->first_dirty_txn)
        return (false);

    /*
     * The problematic case is when a page was evicted but when there were unresolved updates and
     * not every block associated with the page has a disk address. We can't skip such pages because
     * we need a checkpoint write with valid addresses.
     *
     * The page's modification information can change underfoot if the page is being reconciled, so
     * we'd normally serialize with reconciliation before reviewing page-modification information.
     * However, checkpoint is the only valid writer of dirty leaf pages at this point, we skip the
     * lock.
     */
    if (mod->rec_result == WT_PM_REC_MULTIBLOCK)
        for (multi = mod->mod_multi, i = 0; i < mod->mod_multi_entries; ++multi, ++i)
            if (multi->addr.block_cookie == NULL)
                return (false);

    /* RTS, recovery or shutdown should not leave anything dirty behind. */
    if (F_ISSET(session, WT_SESSION_ROLLBACK_TO_STABLE))
        return (false);
    if (F_ISSET(S2C(session), WT_CONN_RECOVERING) ||
      F_ISSET_ATOMIC_32(S2C(session), WT_CONN_CLOSING_CHECKPOINT))
        return (false);

    /*
     * There is no snapshot transaction active. Usually, there is one in ordinary application
     * checkpoints but not all internal cases. Furthermore, this guarantees the metadata file is
     * never skipped.
     */
    if (!F_ISSET(txn, WT_TXN_HAS_SNAPSHOT))
        return (false);

    return (true);
}

/*
 * __sync_dup_hazard_pointer --
 *     Get a duplicate hazard pointer.
 */
static WT_INLINE int
__sync_dup_hazard_pointer(WT_SESSION_IMPL *session, WT_REF *walk)
{
    bool busy;

    /* Get a duplicate hazard pointer. */
    for (;;) {
        /*
         * We already have a hazard pointer, we should generally be able to get another one. We can
         * get spurious busy errors (e.g., if eviction is attempting to lock the page). Keep trying:
         * we have one hazard pointer so we should be able to get another one.
         */
        WT_RET(__wt_hazard_set(session, walk, &busy));
        if (!busy)
            break;
        __wt_yield();
    }
    return (0);
}

/*
 * __sync_dup_walk --
 *     Duplicate a tree walk point.
 */
static WT_INLINE int
__sync_dup_walk(WT_SESSION_IMPL *session, WT_REF *walk, uint32_t flags, WT_REF **dupp)
{
    WT_REF *old;

    if ((old = *dupp) != NULL) {
        *dupp = NULL;
        WT_RET(__wt_page_release(session, old, flags));
    }

    /* It is okay to duplicate a walk before it starts. */
    if (walk == NULL || __wt_ref_is_root(walk)) {
        *dupp = walk;
        return (0);
    }

    WT_RET(__sync_dup_hazard_pointer(session, walk));
    *dupp = walk;
    return (0);
}

/*
 * __wt_sync_file --
 *     Flush pages for a specific file.
 */
int
__wt_sync_file(WT_SESSION_IMPL *session, WT_CACHE_OP syncop)
{
    WT_BTREE *btree;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_PAGE *page;
    WT_PAGE_MODIFY *mod;
    WT_REF *prev, *walk;
    WT_TXN *txn;
    uint64_t internal_bytes, internal_pages, leaf_bytes, leaf_pages;
    uint64_t oldest_id, saved_pinned_id, time_start, time_stop;
    uint32_t flags, rec_flags;
    bool dirty, is_hs, is_internal, tried_eviction;

    conn = S2C(session);
    btree = S2BT(session);
    prev = walk = NULL;
    txn = session->txn;
    tried_eviction = false;

    /* Don't bump page read generations. */
    flags = WT_READ_INTERNAL_OP;

    internal_bytes = leaf_bytes = 0;
    internal_pages = leaf_pages = 0;
    saved_pinned_id = __wt_atomic_loadv64(&WT_SESSION_TXN_SHARED(session)->pinned_id);
    time_start = WT_VERBOSE_ISSET(session, WT_VERB_CHECKPOINT) ? __wt_clock(session) : 0;

    switch (syncop) {
    case WT_SYNC_WRITE_LEAVES:
        /*
         * Write all immediately available, dirty in-cache leaf pages.
         *
         * Writing the leaf pages is done without acquiring a high-level lock, serialize so multiple
         * threads don't walk the tree at the same time.
         */
        if (!btree->modified)
            return (0);
        __wt_spin_lock(session, &btree->flush_lock);
        if (!btree->modified) {
            __wt_spin_unlock(session, &btree->flush_lock);
            return (0);
        }

        /*
         * Save the oldest transaction ID we need to keep around. Otherwise, in a busy system, we
         * could be updating pages so fast that write leaves never catches up. We deliberately have
         * no transaction running at this point that would keep the oldest ID from moving forwards
         * as we walk the tree.
         */
        oldest_id = __wt_txn_oldest_id(session);

        LF_SET(WT_READ_CACHE | WT_READ_NO_WAIT | WT_READ_SKIP_INTL);
        if (!F_ISSET(txn, WT_TXN_HAS_SNAPSHOT))
            LF_SET(WT_READ_VISIBLE_ALL);

        for (;;) {
            WT_ERR(__wt_tree_walk(session, &walk, flags));
            if (walk == NULL)
                break;

            /*
             * Write dirty pages if nobody beat us to it. Don't try to write hot pages (defined as
             * pages that have been updated since the write phase leaves started): checkpoint will
             * have to visit them anyway.
             */
            page = walk->page;
            if (__wt_page_is_modified(page) &&
              __wt_atomic_load64(&page->modify->update_txn) < oldest_id) {
                if (txn->isolation == WT_ISO_READ_COMMITTED)
                    __wt_txn_get_snapshot(session);
                leaf_bytes += __wt_atomic_loadsize(&page->memory_footprint);
                ++leaf_pages;
                WT_ERR(__wt_reconcile(session, walk, NULL, WT_REC_CHECKPOINT));
            }
        }
        break;
    case WT_SYNC_CHECKPOINT:
        /*
         * If we are flushing a file at read-committed isolation, which is of particular interest
         * for flushing the metadata to make a schema-changing operation durable, get a
         * transactional snapshot now.
         *
         * All changes committed up to this point should be included. We don't update the snapshot
         * in between pages because the metadata shouldn't have many pages. Instead, read-committed
         * isolation ensures that all metadata updates completed before the checkpoint are included.
         */
        if (txn->isolation == WT_ISO_READ_COMMITTED)
            __wt_txn_get_snapshot(session);

        /*
         * We cannot check the tree modified flag in the case of a checkpoint, the checkpoint code
         * has already cleared it.
         *
         * Writing the leaf pages is done without acquiring a high-level lock, serialize so multiple
         * threads don't walk the tree at the same time. We're holding the schema lock, but need the
         * lower-level lock as well.
         */
        __wt_spin_lock(session, &btree->flush_lock);

        /*
         * In the final checkpoint pass, child pages cannot be evicted from underneath internal
         * pages nor can underlying blocks be freed until the checkpoint's block lists are stable.
         * Also, we cannot split child pages into parents unless we know the final pass will write a
         * consistent view of that namespace. Set the checkpointing flag to block such actions and
         * wait for any problematic eviction or page splits to complete.
         */
        WT_ASSERT(session,
          __wt_atomic_load_enum(&btree->syncing) == WT_BTREE_SYNC_OFF &&
            __wt_atomic_load_pointer(&btree->sync_session) == NULL);

        __wt_atomic_store_pointer(&btree->sync_session, session);
        __wt_atomic_store_enum(&btree->syncing, WT_BTREE_SYNC_WAIT);
        __wt_gen_next_drain(session, WT_GEN_EVICT);
        __wt_atomic_store_enum(&btree->syncing, WT_BTREE_SYNC_RUNNING);

        /*
         * Reset the number of obsolete time window pages to let the eviction threads and checkpoint
         * cleanup operation to continue marking the clean obsolete time window pages as dirty once
         * the checkpoint is finished.
         */
        __wt_atomic_store32(&btree->eviction_obsolete_tw_pages, 0);
        __wt_atomic_store32(&btree->checkpoint_cleanup_obsolete_tw_pages, 0);
        is_hs = WT_IS_HS(btree->dhandle);

        /* Add in history store reconciliation for standard files. */
        rec_flags = WT_REC_CHECKPOINT;
        if (!is_hs && !WT_IS_METADATA(btree->dhandle))
            rec_flags |= WT_REC_HS;

        /* Write all dirty in-cache pages. */
        LF_SET(WT_READ_NO_EVICT);

        /* Limit reads to cache-only. */
        LF_SET(WT_READ_CACHE);

        if (!F_ISSET(txn, WT_READ_VISIBLE_ALL))
            LF_SET(WT_READ_VISIBLE_ALL);

        for (;;) {
            WT_ERR(__sync_dup_walk(session, walk, flags, &prev));
            WT_ERR(__wt_tree_walk_custom_skip(session, &walk, NULL, NULL, flags));

            if (walk == NULL)
                break;

            is_internal = F_ISSET(walk, WT_REF_FLAG_INTERNAL);
            page = walk->page;

            if (is_internal)
                WT_STAT_CONN_INCR(session, checkpoint_pages_visited_internal);
            else
                WT_STAT_CONN_INCR(session, checkpoint_pages_visited_leaf);

            /*
             * Check if the page is dirty. Add a barrier between the check and taking a reference to
             * any page modify structure. (It needs to be ordered else a page could be dirtied after
             * taking the local reference.)
             */
            dirty = __wt_page_is_modified(page);
            WT_ACQUIRE_BARRIER();

            /* Skip clean pages, but always update the maximum transaction ID and timestamp. */
            if (!dirty) {
                mod = page->modify;
                if (mod != NULL && mod->rec_max_txn > btree->rec_max_txn)
                    btree->rec_max_txn = mod->rec_max_txn;
                if (mod != NULL && btree->rec_max_timestamp < mod->rec_max_timestamp)
                    btree->rec_max_timestamp = mod->rec_max_timestamp;

                continue;
            }

            /*
             * Write dirty pages, if we can't skip them. If we skip a page, mark the tree dirty. The
             * checkpoint marked it clean and we can't skip future checkpoints until this page is
             * written.
             */
            if (__sync_checkpoint_can_skip(session, walk)) {
                __wt_tree_modify_set(session);
                continue;
            }

            if (is_internal) {
                internal_bytes += __wt_atomic_loadsize(&page->memory_footprint);
                ++internal_pages;
                /* Slow down checkpoints. */
                if (FLD_ISSET(conn->debug_flags, WT_CONN_DEBUG_SLOW_CKPT))
                    __wt_sleep(0, 10 * WT_THOUSAND);
            } else {
                leaf_bytes += __wt_atomic_loadsize(&page->memory_footprint);
                ++leaf_pages;
            }

            /*
             * When the timing stress is enabled, perform the leaf page eviction by the checkpoint.
             *
             * For eviction to have a chance, we first need to move the walk point to the next page
             * checkpoint will visit. We want to avoid this code being too special purpose, so try
             * to reuse the ordinary eviction path.
             *
             * Regardless of whether eviction succeeds or fails, the walk continues from the
             * previous location. We remember whether we tried eviction, and don't try again. Even
             * if eviction fails (the page may stay in cache clean), that is not a wasted effort
             * because checkpoint doesn't need to write the page again.
             *
             * Once the transaction has given up it's snapshot it is no longer safe to reconcile
             * pages. That happens prior to the final metadata checkpoint.
             */
            if (!is_internal &&
              FLD_ISSET(conn->timing_stress_flags, WT_TIMING_STRESS_CHECKPOINT_EVICT_PAGE) &&
              !tried_eviction && F_ISSET(session->txn, WT_TXN_HAS_SNAPSHOT)) {
                ret = __wt_page_release_evict(session, walk, 0);
                walk = NULL;
                WT_ERR_ERROR_OK(ret, EBUSY, false);

                walk = prev;
                prev = NULL;
                tried_eviction = true;
                continue;
            }
            tried_eviction = false;

            WT_STAT_CONN_INCR(session, checkpoint_pages_reconciled);
            WT_STAT_CONN_INCRV(session, checkpoint_pages_reconciled_bytes, page->memory_footprint);
            WT_STATP_DSRC_INCR(session, btree->dhandle->stats, btree_checkpoint_pages_reconciled);
            if (FLD_ISSET(rec_flags, WT_REC_HS))
                WT_STAT_CONN_INCR(session, checkpoint_hs_pages_reconciled);

            WT_ERR(__wt_reconcile(session, walk, NULL, rec_flags));

            /* Update checkpoint IO tracking data. */
            if (__wt_checkpoint_verbose_timer_started(session))
                __wt_checkpoint_progress_stats(
                  session, __wt_atomic_loadsize(&page->memory_footprint));
        }

        /*
         * During normal checkpoints, mark the tree dirty if the btree has modifications that are
         * not visible to the checkpoint. There is a drawback in this approach as we compare the
         * btree's maximum transaction id with the checkpoint snap_min and it is possible that this
         * transaction may be visible to the checkpoint, but still, we mark the tree as dirty if
         * there is a long-running transaction in the database.
         *
         * Do not mark the tree dirty if there is no change to stable timestamp compared to the last
         * checkpoint.
         */
        if (!btree->modified && !F_ISSET(conn, WT_CONN_RECOVERING) &&
          !F_ISSET_ATOMIC_32(conn, WT_CONN_CLOSING_CHECKPOINT) &&
          (btree->rec_max_txn >= txn->snapshot_data.snap_min ||
            (conn->txn_global.checkpoint_timestamp != conn->txn_global.last_ckpt_timestamp &&
              btree->rec_max_timestamp > conn->txn_global.checkpoint_timestamp)))
            __wt_tree_modify_set(session);
        break;
    case WT_SYNC_CLOSE:
    case WT_SYNC_DISCARD:
        WT_ERR(__wt_illegal_value(session, syncop));
        break;
    }

    if (time_start != 0) {
        time_stop = __wt_clock(session);
        __wt_verbose_debug2(session, WT_VERB_CHECKPOINT,
          "__sync_file WT_SYNC_%s wrote: %" PRIu64 " leaf pages (%" PRIu64 "B), %" PRIu64
          " internal pages (%" PRIu64 "B), and took %" PRIu64 "ms",
          syncop == WT_SYNC_WRITE_LEAVES ? "WRITE_LEAVES" : "CHECKPOINT", leaf_pages, leaf_bytes,
          internal_pages, internal_bytes, WT_CLOCKDIFF_MS(time_stop, time_start));
    }

err:
    /* On error, clear any left-over tree walk. */
    WT_TRET(__wt_page_release(session, walk, flags));
    WT_TRET(__wt_page_release(session, prev, flags));

    /*
     * If we got a snapshot in order to write pages, and there was no snapshot active when we
     * started, release it.
     */
    if (txn->isolation == WT_ISO_READ_COMMITTED && saved_pinned_id == WT_TXN_NONE)
        __wt_txn_release_snapshot(session);

    /* Clear the checkpoint flag. */
    __wt_atomic_store_enum(&btree->syncing, WT_BTREE_SYNC_OFF);
    __wt_atomic_store_pointer(&btree->sync_session, NULL);

    __wt_spin_unlock(session, &btree->flush_lock);

    /*
     * Leaves are written before a checkpoint (or as part of a file close, before checkpointing the
     * file). Start a flush to stable storage, but don't wait for it.
     */
    if (ret == 0 && syncop == WT_SYNC_WRITE_LEAVES && F_ISSET(conn, WT_CONN_CKPT_SYNC))
        WT_RET(btree->bm->sync(btree->bm, session, false));

    return (ret);
}
