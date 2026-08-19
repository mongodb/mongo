/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __sync_evict_reconciled_under_ckpt_snapshot --
 *     Return true if eviction already reconciled the page using this checkpoint's snapshot, so the
 *     on-disk image checkpoint would produce is identical and re-reconciliation can be skipped.
 */
static WT_INLINE bool
__sync_evict_reconciled_under_ckpt_snapshot(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_CONNECTION_IMPL *conn;
    WT_PAGE_MODIFY *mod;

    conn = S2C(session);
    mod = ref->page->modify;

    if (!F_ISSET(conn, WT_CONN_PRECISE_CHECKPOINT))
        return (false);

    /*
     * The page must have been reconciled under the snapshot this checkpoint published. The
     * connection's checkpoint generation identifies the running one.
     */
    if (mod->rec_ckpt_snap_gen == WT_CKPT_SNAP_GEN_NONE ||
      mod->rec_ckpt_snap_gen != __wt_gen(session, WT_GEN_CHECKPOINT))
        return (false);

    if (mod->rec_pinned_stable_timestamp !=
      __wt_atomic_load_uint64_relaxed(&conn->txn_global.checkpoint_timestamp))
        return (false);

    return (true);
}

/*
 * __sync_page_image_durable --
 *     Return true if every reconciliation product of the page is backed by a written block address,
 *     so checkpoint can leave the existing on-disk image in place instead of rewriting it.
 */
static WT_INLINE bool
__sync_page_image_durable(WT_REF *ref)
{
    WT_MULTI *multi;
    WT_PAGE_MODIFY *mod;
    u_int i;

    mod = ref->page->modify;

    /* A re-instantiated page keeps its written address on the ref. */
    if (mod->rec_result == 0)
        return (__wt_atomic_load_ptr_relaxed(&ref->addr) != NULL);

    switch (mod->rec_result) {
    case WT_PM_REC_EMPTY:
        /* The page is deleted, there is nothing to write. */
        return (true);
    case WT_PM_REC_REPLACE:
        /* The block is written. */
        return (mod->mod_replace.block_cookie != NULL);
    case WT_PM_REC_MULTIBLOCK:
        /*
         * A page evicted with unresolved updates can have blocks without a disk address; checkpoint
         * must write it with valid addresses.
         */
        for (multi = mod->mod_multi, i = 0; i < mod->mod_multi_entries; ++multi, ++i)
            if (multi->addr.block_cookie == NULL)
                return (false);
        return (true);
    default:
        return (false);
    }
}

/*
 * __sync_scrub_checkpoint_enabled --
 *     Return true if checkpoint reconciliation should retain clean disk images for scrub eviction.
 */
static bool
__sync_scrub_checkpoint_enabled(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    /* Skip during recovery or checkpoint shutdown: the scrubbed image would never be consumed. */
    if (F_ISSET(conn, WT_CONN_RECOVERING) || F_ISSET_ATOMIC_32(conn, WT_CONN_CLOSING_CHECKPOINT))
        return (false);

    /* Skip the metadata trees when generating images. */
    if (WT_IS_ANY_METADATA(S2BT(session)->dhandle))
        return (false);

    switch (__wt_atomic_load_uint8_relaxed(
      &conn->cache->cache_eviction_controls.checkpoint_scrub_eviction)) {
    case WT_CACHE_CHECKPOINT_SCRUB_EVICT_OFF:
        return (false);
    case WT_CACHE_CHECKPOINT_SCRUB_EVICT_ON:
        return (true);
    default:
        /* Only retain an image while eviction is in scrub mode. */
        return (
          F_ISSET(conn, WT_CONN_PRECISE_CHECKPOINT) && F_ISSET(conn->evict, WT_EVICT_CACHE_SCRUB));
    }
}

/*
 * __sync_page_rec_flags --
 *     Add a scrub-image request to a page's reconciliation flags. Only a row-store leaf can be
 *     swapped for its image, and the cache budget is re-read per page because pages queued for a
 *     reconciliation worker have not consumed their image yet.
 */
static uint32_t
__sync_page_rec_flags(
  WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t rec_flags, bool checkpoint_scrub)
{
    if (checkpoint_scrub && page->type == WT_PAGE_ROW_LEAF &&
      __wt_cache_scrub_image_budget_ok(session))
        FLD_SET(rec_flags, WT_REC_SAVE_IMAGE_CLEAN);

    return (rec_flags);
}

/*
 * __sync_checkpoint_can_skip --
 *     There are limited conditions under which we can skip writing a dirty page during checkpoint.
 */
static WT_INLINE bool
__sync_checkpoint_can_skip(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_PAGE_MODIFY *mod;
    WT_TXN *txn;

    WT_ASSERT_SPINLOCK_OWNED(session, &S2BT(session)->flush_lock);

    txn = session->txn;
    mod = ref->page->modify;

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

    /*
     * We can only skip writing the page if its current content is already durable on disk. A page
     * evicted with unresolved updates, or re-instantiated from an image that was never written, has
     * content that only exists in memory; checkpoint must write it with valid addresses.
     *
     * The page's modification information can change underfoot if the page is being reconciled, so
     * we'd normally serialize with reconciliation before reviewing page-modification information.
     * However, checkpoint is the only valid writer of dirty leaf pages at this point, we skip the
     * lock.
     */
    if (!__sync_page_image_durable(ref))
        return (false);

    /*
     * If the checkpoint's snapshot does not include the first dirty update on the page, there is no
     * content for this checkpoint to write and we can skip it.
     */
    if (txn->snapshot_data.snap_max < __wt_tsan_suppress_load_uint64(&mod->first_dirty_txn))
        return (true);

    /*
     * Otherwise there is content to write, unless eviction already reconciled the page under this
     * same checkpoint snapshot and pinned stable timestamp; in that case the on-disk image is
     * identical to what checkpoint would produce and we can skip re-reconciliation.
     */
    if (__sync_evict_reconciled_under_ckpt_snapshot(session, ref)) {
        WT_STAT_CONN_INCR(session, checkpoint_pages_reconciliation_skipped_evict_snapshot);
        return (true);
    }

    return (false);
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
 * __sync_check_for_multiblock_rec --
 *     If a page has a pending multiblock split as a result of checkpoint reconciliation, flag it
 *     for eviction. Writing out that split is more efficient than allowing the page to go through
 *     reconciliation again. This also has the desirable effect of increasing the number of deltas
 *     WiredTiger can generate for a workload; essentially, the pages that the original page was
 *     split into can have deltas generated from them.
 */
static void
__sync_check_for_multiblock_rec(WT_SESSION_IMPL *session, WT_REF *walk, bool internal)
{
    WT_PAGE *page = walk->page;

    if (internal || !F_ISSET(S2BT(session), WT_BTREE_DISAGGREGATED) ||
      !WT_REC_RESULT_MULTIBLOCK_SPLIT(page))
        return;

    WT_STAT_CONN_DSRC_INCR(session, cache_eviction_multiblock_checkpoint_flagged);
    __wt_evict_page_soon(session, walk);
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
    uint64_t internal_bytes, internal_pages, leaf_bytes, leaf_pages, oldest_id;
    uint64_t reconcile_time_pct, reconcile_time, reconcile_start;
    uint64_t saved_pinned_id, t, time_start, time_stop;
    uint32_t flags, rec_flags;
    bool checkpoint_scrub, dirty, is_hs, is_internal, tried_eviction;

    conn = S2C(session);
    btree = S2BT(session);
    prev = walk = NULL;
    txn = session->txn;
    tried_eviction = false;

    /* Don't bump page read generations. */
    flags = WT_READ_INTERNAL_OP;

    /*
     * The scrub-eviction configuration is fixed for the checkpoint, but whether a given page gets
     * an image also depends on its type and on the cache budget, so that is decided per page.
     */
    checkpoint_scrub = __sync_scrub_checkpoint_enabled(session);

    internal_bytes = leaf_bytes = 0;
    internal_pages = leaf_pages = 0;
    reconcile_time = 0;
    saved_pinned_id = __wt_atomic_load_uint64_v_relaxed(&WT_SESSION_TXN_SHARED(session)->pinned_id);
    time_start = __wt_clock(session);

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

        rec_flags = WT_REC_CHECKPOINT;

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
              __wt_atomic_load_uint64_relaxed(&page->modify->update_txn) < oldest_id) {
                if (txn->isolation == WT_ISO_READ_COMMITTED)
                    __wt_txn_get_snapshot(session);
                leaf_bytes += __wt_atomic_load_size_relaxed(&page->memory_footprint);
                ++leaf_pages;
                reconcile_start = __wt_clock(session);
                WT_ERR(__wt_reconcile(session, walk, NULL,
                  __sync_page_rec_flags(session, page, rec_flags, checkpoint_scrub), NULL));
                reconcile_time += __wt_clock(session) - reconcile_start;
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
        WT_ASSERT(session, __wt_atomic_load_enum_relaxed(&btree->syncing) == WT_BTREE_SYNC_OFF);

        session->syncing = true;
        __wt_atomic_store_enum_release(&btree->syncing, WT_BTREE_SYNC_WAIT);
        __wt_gen_next_drain(session, WT_GEN_EVICT);
        __wt_atomic_store_enum_release(&btree->syncing, WT_BTREE_SYNC_RUNNING);

        /*
         * Reset the number of obsolete time window pages to let the eviction threads and checkpoint
         * cleanup operation to continue marking the clean obsolete time window pages as dirty once
         * the checkpoint is finished.
         */
        __wt_atomic_store_uint32_relaxed(&btree->eviction_obsolete_tw_pages, 0);
        __wt_atomic_store_uint32_relaxed(&btree->checkpoint_cleanup_obsolete_tw_pages, 0);
        is_hs = WT_IS_HS(btree->dhandle);

        /* Add in history store reconciliation for standard files. */
        rec_flags = WT_REC_CHECKPOINT;
        if (!is_hs && !WT_IS_ANY_METADATA(btree->dhandle))
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
            if (WT_SESSION_IS_CHECKPOINT(session))
                ++conn->ckpt.progress.pages_visited;

            /*
             * Wait for the leaf pages to finish reconciling before checking whether the internal
             * page is dirty, as reconciling the leaf pages could have made the internal page dirty.
             */
            if (WT_PARALLEL_CHECKPOINTS_ENABLED(session))
                if (WT_SESSION_IS_CHECKPOINT(session) && is_internal) {
                    WT_ERR(__wt_checkpoint_parallel_finish(session, &t));
                    reconcile_time += t;
                }

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

                /* Handle unresolved multiblock reconciliations that we see along the way. */
                __sync_check_for_multiblock_rec(session, walk, is_internal);
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
                internal_bytes += __wt_atomic_load_size_relaxed(&page->memory_footprint);
                ++internal_pages;
                /* Slow down checkpoints. */
                if (FLD_ISSET(conn->debug.flags, WT_CONN_DEBUG_SLOW_CKPT))
                    __wt_sleep(0, 10 * WT_THOUSAND);
            } else {
                leaf_bytes += __wt_atomic_load_size_relaxed(&page->memory_footprint);
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
            WT_STAT_CONN_INCRV(session, checkpoint_pages_reconciled_bytes,
              __wt_atomic_load_size_relaxed(&page->memory_footprint));
            WT_STATP_DSRC_INCR(session, btree->dhandle->stats, btree_checkpoint_pages_reconciled);
            if (WT_IS_HS(btree->dhandle))
                WT_STAT_CONN_INCR(session, checkpoint_hs_pages_reconciled);

            /* Reconcile leaf pages in parallel, waiting at each internal page. */
            if (WT_PARALLEL_CHECKPOINTS_ENABLED(session) && WT_SESSION_IS_CHECKPOINT(session) &&
              !is_internal) {
                /*
                 * Duplicate the position, and give it to the parallel checkpoint worker. The
                 * existing walk position will be release by the walk code.
                 */
                WT_REF *walk_dup = NULL;
                WT_ERR(__sync_dup_walk(session, walk, 0, &walk_dup));
                WT_ERR(__wt_checkpoint_parallel_push_work(session, walk_dup,
                  __sync_page_rec_flags(session, page, rec_flags, checkpoint_scrub), flags));
            } else {
                reconcile_start = __wt_clock(session);
                WT_ERR(__wt_reconcile(session, walk, NULL,
                  __sync_page_rec_flags(session, page, rec_flags, checkpoint_scrub), NULL));
                reconcile_time += __wt_clock(session) - reconcile_start;
            }

            /*
             * Handle unresolved multiblock reconciliations. Some of these will be pages left dirty
             * by checkpoint. Which means eviction will still not be able to evict them, however it
             * can still realize the split and avoid checkpoint splitting the page again.
             */
            __sync_check_for_multiblock_rec(session, walk, is_internal);

            /*
             * Update checkpoint IO tracking data for the session running the checkpoint. Other
             * session can execute this code but we are not tracking their progress.
             */
            if (WT_SESSION_IS_CHECKPOINT(session) && __wt_checkpoint_verbose_timer_started(session))
                __wt_checkpoint_progress_stats(
                  session, __wt_atomic_load_size_relaxed(&page->memory_footprint));
        }

        /* Wait for the workers to finish; we need this if the root page is also a leaf page. */
        if (WT_PARALLEL_CHECKPOINTS_ENABLED(session) && WT_SESSION_IS_CHECKPOINT(session)) {
            WT_ERR(__wt_checkpoint_parallel_finish(session, &t));
            reconcile_time += t;
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
         *
         * The load is relaxed rather than acquire: this runs on the checkpoint thread, under the
         * checkpoint lock, which is the same thread that publishes the timestamp, and the value is
         * only compared to decide whether to mark the tree dirty. No state published alongside the
         * timestamp is consumed here.
         */
        if (!btree->modified && !F_ISSET(conn, WT_CONN_RECOVERING) &&
          !F_ISSET_ATOMIC_32(conn, WT_CONN_CLOSING_CHECKPOINT) &&
          (btree->rec_max_txn >= txn->snapshot_data.snap_min ||
            (conn->txn_global.checkpoint_timestamp !=
                __wt_atomic_load_uint64_relaxed(&conn->txn_global.last_ckpt_timestamp) &&
              btree->rec_max_timestamp > conn->txn_global.checkpoint_timestamp)))
            __wt_tree_modify_set(session);
        break;
    case WT_SYNC_CLOSE:
    case WT_SYNC_DISCARD:
        WT_ERR(__wt_illegal_value(session, syncop));
        break;
    }

    /* Calculate and log sync efficiency statistics for checkpoints. */
    if (WT_SESSION_IS_CHECKPOINT(session)) {
        time_stop = __wt_clock(session);
        if (time_stop != time_start)
            reconcile_time_pct = (reconcile_time * 100) / (time_stop - time_start);
        else
            reconcile_time_pct = 0;
        __wt_verbose_debug2(session, WT_VERB_CHECKPOINT,
          "__sync_file WT_SYNC_%s wrote: %" PRIu64 " leaf pages (%" PRIu64 "B), %" PRIu64
          " internal pages (%" PRIu64 "B), and took %" PRIu64 "ms",
          syncop == WT_SYNC_WRITE_LEAVES ? "WRITE_LEAVES" : "CHECKPOINT", leaf_pages, leaf_bytes,
          internal_pages, internal_bytes, WT_CLOCKDIFF_MS(time_stop, time_start));
        __wt_verbose_debug2(session, WT_VERB_CHECKPOINT,
          "__sync_file WT_SYNC_%s spent %" PRIu64 "ms in reconciliation across %" PRIu32
          " threads (%" PRIu64 "%% of the wall-clock time)",
          syncop == WT_SYNC_WRITE_LEAVES ? "WRITE_LEAVES" : "CHECKPOINT",
          WT_CLOCKDIFF_MS(reconcile_time, 0), WT_PARALLEL_CHECKPOINTS_NUM_THREADS(session),
          reconcile_time_pct);
        __wt_checkpoint_rec_time_stats(session, reconcile_time, time_stop - time_start);
    }

err:
    /* On error, clear any left-over tree walk. */
    WT_TRET(__wt_page_release(session, walk, flags));
    WT_TRET(__wt_page_release(session, prev, flags));

    /*
     * Wait for the workers to finish, as they may be still doing work if we got here because of an
     * error.
     */
    if (WT_PARALLEL_CHECKPOINTS_ENABLED(session) && WT_SESSION_IS_CHECKPOINT(session))
        WT_TRET(__wt_checkpoint_parallel_finish(session, NULL));

    /*
     * If we got a snapshot in order to write pages, and there was no snapshot active when we
     * started, release it.
     */
    if (txn->isolation == WT_ISO_READ_COMMITTED && saved_pinned_id == WT_TXN_NONE)
        __wt_txn_release_snapshot(session);

    if (syncop == WT_SYNC_CHECKPOINT) {
        /*
         * Ensure the checkpoint generation is updated before clearing the sync flag. Otherwise,
         * eviction could evict a page from the btree after the flag is cleared but before the
         * checkpoint generation is updated. This would violate the constraints of disaggregated
         * storage, as eviction would write a page that should not be part of the current
         * checkpoint.
         */
        __wt_checkpoint_update_generation(session, btree);

        /* Clear the checkpoint flag. */
        __wt_atomic_store_enum_release(&btree->syncing, WT_BTREE_SYNC_OFF);
        session->syncing = false;
    }

    __wt_spin_unlock(session, &btree->flush_lock);

    /*
     * Leaves are written before a checkpoint (or as part of a file close, before checkpointing the
     * file). Start a flush to stable storage, but don't wait for it.
     */
    if (ret == 0 && syncop == WT_SYNC_WRITE_LEAVES && F_ISSET(conn, WT_CONN_CKPT_SYNC))
        WT_RET(btree->bm->sync(btree->bm, session, false));

    return (ret);
}
