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
static inline bool
__sync_checkpoint_can_skip(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_MULTI *multi;
    WT_PAGE_MODIFY *mod;
    WT_TXN *txn;
    u_int i;

    mod = ref->page->modify;
    txn = session->txn;

    /*
     * We can skip some dirty pages during a checkpoint. The requirements:
     *
     * 1. Not a history btree. As part of the checkpointing the data store, we will move the older
     *    values into the history store without using any transactions. This led to representation
     *    of all the modifications on the history store page with a transaction that is maximum than
     *    the checkpoint snapshot. But these modifications are done by the checkpoint itself, so we
     *    shouldn't ignore them for consistency.
     * 2. they must be leaf pages,
     * 3. there is a snapshot transaction active (which is the case in ordinary application
     *    checkpoints but not all internal cases),
     * 4. the first dirty update on the page is sufficiently recent the checkpoint transaction would
     *     skip them,
     * 5. there's already an address for every disk block involved.
     */
    if (WT_IS_HS(session->dhandle))
        return (false);
    if (F_ISSET(ref, WT_REF_FLAG_INTERNAL))
        return (false);
    if (!F_ISSET(txn, WT_TXN_HAS_SNAPSHOT))
        return (false);
    if (!WT_TXNID_LT(txn->snap_max, mod->first_dirty_txn))
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
            if (multi->addr.addr == NULL)
                return (false);

    return (true);
}

/*
 * __sync_dup_hazard_pointer --
 *     Get a duplicate hazard pointer.
 */
static inline int
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
static inline int
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
 * __sync_ref_obsolete_check --
 *     Check whether the ref is obsolete according to the newest stop time point and handle the
 *     obsolete page.
 */
static int
__sync_ref_obsolete_check(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_ADDR_COPY addr;
    WT_DECL_RET;
    WT_MULTI *multi;
    WT_PAGE_MODIFY *mod;
    wt_timestamp_t newest_stop_ts, newest_stop_durable_ts;
    uint64_t newest_stop_txn;
    uint32_t i;
    uint8_t previous_state;
    char tp_string[WT_TP_STRING_SIZE];
    const char *tag;
    bool busy, hazard, obsolete, ovfl_items;

    /* Ignore root pages as they can never be deleted. */
    if (__wt_ref_is_root(ref)) {
        __wt_verbose(session, WT_VERB_CHECKPOINT_CLEANUP, "%p: skipping root page", (void *)ref);
        return (0);
    }

    /* Ignore internal pages, these are taken care of during reconciliation. */
    if (F_ISSET(ref, WT_REF_FLAG_INTERNAL)) {
        __wt_verbose(session, WT_VERB_CHECKPOINT_CLEANUP,
          "%p: skipping internal page with parent: %p", (void *)ref, (void *)ref->home);
        return (0);
    }

    /* Fast-check, ignore deleted pages. */
    if (ref->state == WT_REF_DELETED) {
        __wt_verbose(session, WT_VERB_CHECKPOINT_CLEANUP, "%p: skipping deleted page", (void *)ref);
        return (0);
    }

    /* Lock the WT_REF. */
    WT_REF_LOCK(session, ref, &previous_state);

    /*
     * If the page is on-disk and obsolete, mark the page as deleted and also set the parent page as
     * dirty. This is to ensure the parent is written during the checkpoint and the child page
     * discarded.
     */
    newest_stop_ts = WT_TS_NONE;
    newest_stop_durable_ts = WT_TS_NONE;
    newest_stop_txn = WT_TXN_NONE;
    obsolete = false;
    ovfl_items = false;
    if (previous_state == WT_REF_DISK) {
        /*
         * There should be an address, but simply skip any page where we don't find one. Also skip
         * the pages that have overflow keys as part of fast delete flow. These overflow keys pages
         * are handled as an in-memory obsolete page flow.
         */
        if (__wt_ref_addr_copy(session, ref, &addr) && addr.type == WT_ADDR_LEAF_NO) {
            /*
             * Max stop timestamp is possible only when the prepared update is written to the data
             * store.
             */
            newest_stop_ts = addr.ta.newest_stop_ts;
            newest_stop_durable_ts =
              addr.ta.newest_stop_ts == WT_TS_MAX ? WT_TS_MAX : addr.ta.newest_stop_durable_ts;
            newest_stop_txn = addr.ta.newest_stop_txn;
            obsolete = __wt_txn_visible_all(session, newest_stop_txn, newest_stop_durable_ts);
        }

        if (obsolete) {
            WT_REF_UNLOCK(ref, WT_REF_DELETED);
            WT_STAT_CONN_DATA_INCR(session, cc_pages_removed);

            WT_RET(__wt_page_parent_modify_set(session, ref, true));
        } else
            WT_REF_UNLOCK(ref, previous_state);

        __wt_verbose(session, WT_VERB_CHECKPOINT_CLEANUP,
          "%p on-disk page obsolete check: %s"
          "obsolete, stop time point %s",
          (void *)ref, obsolete ? "" : "not ",
          __wt_time_point_to_string(
            newest_stop_ts, newest_stop_durable_ts, newest_stop_txn, tp_string));
        return (0);
    }
    WT_REF_UNLOCK(ref, previous_state);

    /*
     * Ignore pages that aren't in-memory for some reason other than they're on-disk, for example,
     * they might have split or been deleted while we were locking the WT_REF.
     */
    if (previous_state != WT_REF_MEM) {
        __wt_verbose(session, WT_VERB_CHECKPOINT_CLEANUP, "%p: skipping page", (void *)ref);
        return (0);
    }

    /*
     * Reviewing in-memory pages requires looking at page reconciliation results and we must ensure
     * we don't race with page reconciliation as it's writing the page modify information. There are
     * two ways we call reconciliation: checkpoints and eviction. We are the checkpoint thread so
     * that's not a problem, acquire a hazard pointer to prevent page eviction. If the page is in
     * transition or switches state (we've already released our lock), just walk away, we'll deal
     * with it next time.
     */
    WT_RET(__wt_hazard_set(session, ref, &busy));
    if (busy)
        return (0);
    hazard = true;

    mod = ref->page == NULL ? NULL : ref->page->modify;
    if (mod != NULL && mod->rec_result == WT_PM_REC_EMPTY) {
        tag = "reconciled empty";

        obsolete = true;
    } else if (mod != NULL && mod->rec_result == WT_PM_REC_MULTIBLOCK) {
        tag = "reconciled multi-block";

        /* Calculate the max stop time point by traversing all multi addresses. */
        for (multi = mod->mod_multi, i = 0; i < mod->mod_multi_entries; ++multi, ++i) {
            newest_stop_txn = WT_MAX(newest_stop_txn, multi->addr.ta.newest_stop_txn);
            newest_stop_ts = WT_MAX(newest_stop_ts, multi->addr.ta.newest_stop_ts);
            newest_stop_durable_ts = WT_MAX(newest_stop_durable_ts,
              multi->addr.ta.newest_stop_ts == WT_TS_MAX ? WT_TS_MAX :
                                                           multi->addr.ta.newest_stop_durable_ts);
            if (multi->addr.type == WT_ADDR_LEAF)
                ovfl_items = true;
        }
        obsolete = __wt_txn_visible_all(session, newest_stop_txn, newest_stop_durable_ts);
    } else if (mod != NULL && mod->rec_result == WT_PM_REC_REPLACE) {
        tag = "reconciled replacement block";

        newest_stop_txn = mod->mod_replace.ta.newest_stop_txn;
        newest_stop_ts = mod->mod_replace.ta.newest_stop_ts;
        newest_stop_durable_ts = mod->mod_replace.ta.newest_stop_ts == WT_TS_MAX ?
          WT_TS_MAX :
          mod->mod_replace.ta.newest_stop_durable_ts;
        if (mod->mod_replace.type == WT_ADDR_LEAF)
            ovfl_items = true;
        obsolete = __wt_txn_visible_all(session, newest_stop_txn, newest_stop_durable_ts);
    } else if (__wt_ref_addr_copy(session, ref, &addr)) {
        tag = "WT_REF address";

        newest_stop_txn = addr.ta.newest_stop_txn;
        newest_stop_ts = addr.ta.newest_stop_ts;
        newest_stop_durable_ts =
          addr.ta.newest_stop_ts == WT_TS_MAX ? WT_TS_MAX : addr.ta.newest_stop_durable_ts;
        if (addr.type == WT_ADDR_LEAF)
            ovfl_items = true;
        obsolete = __wt_txn_visible_all(session, newest_stop_txn, newest_stop_durable_ts);
    } else
        tag = "unexpected page state";

    if (obsolete) {
        /*
         * Dirty the obsolete page with overflow items to let the page reconciliation remove all the
         * overflow items.
         */
        if (ovfl_items) {
            WT_ERR(__wt_page_modify_init(session, ref->page));
            __wt_page_modify_set(session, ref->page);
        }

        /*
         * Set the obsolete page read generation number to a lower value indicating as it is no
         * longer needed for any read operations to let the eviction to evict it sooner.
         */
        ref->page->read_gen = WT_READGEN_WONT_NEED;
        WT_STAT_CONN_DATA_INCR(session, cc_pages_evict);
    }

    __wt_verbose(session, WT_VERB_CHECKPOINT_CLEANUP,
      "%p in-memory page obsolete check: %s %s"
      "obsolete, stop time point %s",
      (void *)ref, tag, obsolete ? "" : "not ",
      __wt_time_point_to_string(
        newest_stop_ts, newest_stop_durable_ts, newest_stop_txn, tp_string));

err:
    if (hazard)
        WT_TRET(__wt_hazard_clear(session, ref));
    return (ret);
}

/*
 * __sync_ref_int_obsolete_cleanup --
 *     Traverse the internal page and identify the leaf pages that are obsolete and mark them as
 *     deleted.
 */
static int
__sync_ref_int_obsolete_cleanup(WT_SESSION_IMPL *session, WT_REF *parent)
{
    WT_PAGE_INDEX *pindex;
    WT_REF *ref;
    uint32_t slot;

    __wt_verbose(session, WT_VERB_CHECKPOINT_CLEANUP,
      "%p: traversing the internal page %p for obsolete child pages", (void *)parent,
      (void *)parent->page);

    WT_INTL_INDEX_GET(session, parent->page, pindex);
    for (slot = 0; slot < pindex->entries; slot++) {
        ref = pindex->index[slot];

        WT_RET(__sync_ref_obsolete_check(session, ref));
    }

    WT_STAT_CONN_DATA_INCRV(session, cc_pages_visited, pindex->entries);

    return (0);
}

/*
 * __sync_page_skip --
 *     Return if checkpoint requires we read this page.
 */
static int
__sync_page_skip(
  WT_SESSION_IMPL *session, WT_REF *ref, void *context, bool visible_all, bool *skipp)
{
    WT_ADDR_COPY addr;

    WT_UNUSED(context);
    WT_UNUSED(visible_all);

    *skipp = false; /* Default to reading */

    /* If the page is in-memory, we want to look at it. */
    if (ref->state != WT_REF_DISK)
        return (0);

    /*
     * Reading any page that is not in the cache will increase the cache size. Perform a set of
     * checks to verify the cache can handle it.
     */
    if (__wt_cache_aggressive(session) || __wt_cache_full(session) || __wt_cache_stuck(session) ||
      __wt_eviction_needed(session, false, false, NULL)) {
        *skipp = true;
        return (0);
    }

    /* Don't read pages into cache during startup or shutdown phase. */
    if (F_ISSET(S2C(session), WT_CONN_RECOVERING | WT_CONN_CLOSING_CHECKPOINT)) {
        *skipp = true;
        return (0);
    }

    /*
     * Ignore the pages with no on-disk address. It is possible that a page with deleted state may
     * not have an on-disk address.
     */
    if (!__wt_ref_addr_copy(session, ref, &addr))
        return (0);

    /*
     * Skip reading the pages that are normal leaf pages or don't have an aggregated durable stop
     * timestamp.
     */
    if (addr.type == WT_ADDR_LEAF_NO || addr.ta.newest_stop_durable_ts == WT_TS_NONE) {
        __wt_verbose(session, WT_VERB_CHECKPOINT_CLEANUP, "%p: page walk skipped", (void *)ref);
        WT_STAT_CONN_DATA_INCR(session, cc_pages_walk_skipped);
        *skipp = true;
    }
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
    bool dirty, internal_cleanup, is_hs, tried_eviction;

    conn = S2C(session);
    btree = S2BT(session);
    prev = walk = NULL;
    txn = session->txn;
    tried_eviction = false;

    /* Don't bump page read generations. */
    flags = WT_READ_NO_GEN;

    internal_bytes = leaf_bytes = 0;
    internal_pages = leaf_pages = 0;
    saved_pinned_id = WT_SESSION_TXN_SHARED(session)->pinned_id;
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
            if (__wt_page_is_modified(page) && WT_TXNID_LT(page->modify->update_txn, oldest_id)) {
                if (txn->isolation == WT_ISO_READ_COMMITTED)
                    __wt_txn_get_snapshot(session);
                leaf_bytes += page->memory_footprint;
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
        WT_ASSERT(session, btree->syncing == WT_BTREE_SYNC_OFF && btree->sync_session == NULL);

        btree->sync_session = session;
        btree->syncing = WT_BTREE_SYNC_WAIT;
        __wt_gen_next_drain(session, WT_GEN_EVICT);
        btree->syncing = WT_BTREE_SYNC_RUNNING;
        is_hs = WT_IS_HS(btree->dhandle);

        /* Add in history store reconciliation for standard files. */
        rec_flags = WT_REC_CHECKPOINT;
        if (!is_hs && !WT_IS_METADATA(btree->dhandle))
            rec_flags |= WT_REC_HS;

        /* Write all dirty in-cache pages. */
        LF_SET(WT_READ_NO_EVICT);

        /* Read pages with history store entries and evict them asap. */
        LF_SET(WT_READ_WONT_NEED);

        /*
         * Perform checkpoint cleanup when not in startup or shutdown phase by traversing internal
         * pages looking for obsolete child pages. This is row-store specific, column-store pages
         * cannot be discarded and must be rewritten as they contain chunks of the name space. For
         * the same reason, only read in-memory pages when doing column-store checkpoints (row-store
         * reads all of the internal pages to improve cleanup).
         */
        if (btree->type == BTREE_ROW)
            internal_cleanup = !F_ISSET(conn, WT_CONN_RECOVERING | WT_CONN_CLOSING_CHECKPOINT);
        else {
            LF_SET(WT_READ_CACHE);
            internal_cleanup = false;
        }

        if (!F_ISSET(txn, WT_READ_VISIBLE_ALL))
            LF_SET(WT_READ_VISIBLE_ALL);

        for (;;) {
            WT_ERR(__sync_dup_walk(session, walk, flags, &prev));
            WT_ERR(__wt_tree_walk_custom_skip(session, &walk, __sync_page_skip, NULL, flags));

            if (walk == NULL)
                break;

            if (F_ISSET(walk, WT_REF_FLAG_INTERNAL) && internal_cleanup) {
                WT_WITH_PAGE_INDEX(session, ret = __sync_ref_int_obsolete_cleanup(session, walk));
                WT_ERR(ret);
            }

            page = walk->page;

            /*
             * Check if the page is dirty. Add a barrier between the check and taking a reference to
             * any page modify structure. (It needs to be ordered else a page could be dirtied after
             * taking the local reference.)
             */
            WT_ORDERED_READ(dirty, __wt_page_is_modified(page));

            /* Skip clean pages, but always update the maximum transaction ID. */
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

            if (F_ISSET(walk, WT_REF_FLAG_INTERNAL)) {
                internal_bytes += page->memory_footprint;
                ++internal_pages;
                /* Slow down checkpoints. */
                if (FLD_ISSET(conn->debug_flags, WT_CONN_DEBUG_SLOW_CKPT))
                    __wt_sleep(0, 10000);
            } else {
                leaf_bytes += page->memory_footprint;
                ++leaf_pages;
            }

            /*
             * If the page was pulled into cache by our read, try to evict it now.
             *
             * For eviction to have a chance, we first need to move the walk point to the next page
             * checkpoint will visit. We want to avoid this code being too special purpose, so try
             * to reuse the ordinary eviction path.
             *
             * Regardless of whether eviction succeeds or fails, the walk continues from the
             * previous location. We remember whether we tried eviction, and don't try again. Even
             * if eviction fails (the page may stay in cache clean but with history that cannot be
             * discarded), that is not wasted effort because checkpoint doesn't need to write the
             * page again.
             *
             * Once the transaction has given up it's snapshot it is no longer safe to reconcile
             * pages. That happens prior to the final metadata checkpoint.
             */
            if (F_ISSET(walk, WT_REF_FLAG_LEAF) && page->read_gen == WT_READGEN_WONT_NEED &&
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

            WT_ERR(__wt_reconcile(session, walk, NULL, rec_flags));

            /*
             * Update checkpoint IO tracking data if configured to log verbose progress messages.
             */
            if (conn->ckpt_timer_start.tv_sec > 0) {
                conn->ckpt_write_bytes += page->memory_footprint;
                ++conn->ckpt_write_pages;

                /* Periodically log checkpoint progress. */
                if (conn->ckpt_write_pages % 5000 == 0)
                    __wt_checkpoint_progress(session, false);
            }
        }
        break;
    case WT_SYNC_CLOSE:
    case WT_SYNC_DISCARD:
        WT_ERR(__wt_illegal_value(session, syncop));
        break;
    }

    if (time_start != 0) {
        time_stop = __wt_clock(session);
        __wt_verbose(session, WT_VERB_CHECKPOINT,
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
    btree->syncing = WT_BTREE_SYNC_OFF;
    btree->sync_session = NULL;

    __wt_spin_unlock(session, &btree->flush_lock);

    /*
     * Leaves are written before a checkpoint (or as part of a file close, before checkpointing the
     * file). Start a flush to stable storage, but don't wait for it.
     */
    if (ret == 0 && syncop == WT_SYNC_WRITE_LEAVES && F_ISSET(conn, WT_CONN_CKPT_SYNC))
        WT_RET(btree->bm->sync(btree->bm, session, false));

    return (ret);
}
