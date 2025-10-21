/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __evict_page_clean_update(WT_SESSION_IMPL *, WT_REF *, uint32_t);
static int __evict_page_dirty_update(WT_SESSION_IMPL *, WT_REF *, uint32_t);
static int __evict_reconcile(WT_SESSION_IMPL *, WT_REF *, uint32_t);
static int __evict_review(WT_SESSION_IMPL *, WT_REF *, uint32_t, bool *);

/*
 * __evict_exclusive_clear --
 *     Release exclusive access to a page.
 */
static WT_INLINE void
__evict_exclusive_clear(WT_SESSION_IMPL *session, WT_REF *ref, WT_REF_STATE previous_state)
{
    WT_ASSERT(session, WT_REF_GET_STATE(ref) == WT_REF_LOCKED && ref->page != NULL);

    WT_REF_SET_STATE(ref, previous_state);
}

/*
 * __evict_exclusive --
 *     Acquire exclusive access to a page.
 */
static WT_INLINE int
__evict_exclusive(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_ASSERT(session, WT_REF_GET_STATE(ref) == WT_REF_LOCKED);

    /*
     * Check for a hazard pointer indicating another thread is using the page, meaning the page
     * cannot be evicted.
     */
    if (__wt_hazard_check(session, ref, NULL) == NULL)
        return (0);

    WT_STAT_CONN_DSRC_INCR(session, cache_eviction_blocked_hazard);
    return (__wt_set_return(session, EBUSY));
}

#define WT_EVICT_STATS_CLEAN 0x01
#define WT_EVICT_STATS_FORCE_HS 0x02
#define WT_EVICT_STATS_SUCCESS 0x04
#define WT_EVICT_STATS_URGENT 0x08

/*
 * __evict_stats_update --
 *     Update the stats of eviction.
 *
 */
static void
__evict_stats_update(WT_SESSION_IMPL *session, uint8_t flags)
{
    WT_CONNECTION_IMPL *conn;
    uint64_t eviction_time, eviction_time_milliseconds;

    conn = S2C(session);

    if (session->evict_timeline.reentry_hs_eviction) {
        session->evict_timeline.reentry_hs_evict_finish = __wt_clock(session);
        eviction_time = WT_CLOCKDIFF_US(session->evict_timeline.reentry_hs_evict_finish,
          session->evict_timeline.reentry_hs_evict_start);
    } else {
        session->evict_timeline.evict_finish = __wt_clock(session);
        eviction_time = WT_CLOCKDIFF_US(
          session->evict_timeline.evict_finish, session->evict_timeline.evict_start);
    }
    if (LF_ISSET(WT_EVICT_STATS_SUCCESS)) {
        if (LF_ISSET(WT_EVICT_STATS_URGENT)) {
            if (LF_ISSET(WT_EVICT_STATS_FORCE_HS))
                WT_STAT_CONN_INCR(session, eviction_force_hs_success);
            if (LF_ISSET(WT_EVICT_STATS_CLEAN))
                WT_STAT_CONN_INCR(session, eviction_force_clean);
            else
                WT_STAT_CONN_INCR(session, eviction_force_dirty);
        }

        if (LF_ISSET(WT_EVICT_STATS_CLEAN))
            WT_STAT_CONN_DSRC_INCR(session, cache_eviction_clean);
        else
            WT_STAT_CONN_DSRC_INCR(session, cache_eviction_dirty);

        /* Count page evictions in parallel with checkpoint. */
        if (__wt_atomic_loadvbool(&conn->txn_global.checkpoint_running))
            WT_STAT_CONN_INCR(session, eviction_pages_in_parallel_with_checkpoint);
    } else {
        if (LF_ISSET(WT_EVICT_STATS_URGENT)) {
            if (LF_ISSET(WT_EVICT_STATS_FORCE_HS))
                WT_STAT_CONN_INCR(session, eviction_force_hs_fail);
            WT_STAT_CONN_INCR(session, eviction_force_fail);
        }

        WT_STAT_CONN_DSRC_INCR(session, eviction_fail);
    }
    if (!session->evict_timeline.reentry_hs_eviction) {
        eviction_time_milliseconds = eviction_time / WT_THOUSAND;
        if (eviction_time_milliseconds >
          __wt_atomic_load64(&conn->evict->evict_max_ms_per_checkpoint))
            __wt_atomic_store64(
              &conn->evict->evict_max_ms_per_checkpoint, eviction_time_milliseconds);
        if (eviction_time_milliseconds > __wt_atomic_load64(&conn->evict->evict_max_ms))
            __wt_atomic_store64(&conn->evict->evict_max_ms, eviction_time_milliseconds);
        if (eviction_time_milliseconds > WT_MINUTE * WT_THOUSAND)
            __wt_verbose_warning(session, WT_VERB_EVICTION,
              "Eviction took more than 1 minute (%" PRIu64 "us). Building disk image took %" PRIu64
              "us. History store wrapup took %" PRIu64 "us.",
              eviction_time,
              WT_CLOCKDIFF_US(session->reconcile_timeline.image_build_finish,
                session->reconcile_timeline.image_build_start),
              WT_CLOCKDIFF_US(session->reconcile_timeline.hs_wrapup_finish,
                session->reconcile_timeline.hs_wrapup_start));
    } else {
        /*
         * We are in the reentrant history store eviction inside a data store reconciliation. Add to
         * the total time taken to do the reentrant history store eviction.
         */
        session->reconcile_timeline.total_reentry_hs_eviction_time +=
          WT_CLOCKDIFF_MS(session->evict_timeline.reentry_hs_evict_finish,
            session->evict_timeline.reentry_hs_evict_start);
        session->evict_timeline.reentry_hs_eviction = false;
    }
}

/* !!!
 * __wt_evict --
 *     Evict a page from memory by taking exclusive access to the page.
 *
 *     Based on the page's state, the function either reconciles and writes the page to disk or
 *     simply discards it from the cache. It is called by both eviction worker threads and
 *     application threads.
 *
 *     Input parameters:
 *       (1) `ref`: Reference to the page getting evicted.
 *       (2) `previous_state`: Previous state of the page's reference, restored if the page cannot
 *           be evicted.
 *       (3) `flags`: Eviction-related flags indicating conditions such as `urgent eviction`,
 *           `no splits`, or `tree closing`.
 *
 *     Return an error code for cases blocking exclusive access to the page, failure in
 *     reconciliation, or certain conditions preventing the page's eviction.
 */
int
__wt_evict(WT_SESSION_IMPL *session, WT_REF *ref, WT_REF_STATE previous_state, uint32_t flags)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_PAGE *page;
    uint64_t page_size;
    uint8_t stats_flags;
    bool clean_page, closing, ebusy_only, inmem_split, is_dirty, tree_dead;

    conn = S2C(session);
    page = ref->page;
    closing = LF_ISSET(WT_EVICT_CALL_CLOSING);
    stats_flags = 0;
    clean_page = ebusy_only = is_dirty = false;

    __wt_verbose_debug3(
      session, WT_VERB_EVICTION, "page %p (%s)", (void *)page, __wt_page_type_string(page->type));

    tree_dead = F_ISSET(session->dhandle, WT_DHANDLE_DEAD);
    if (tree_dead)
        LF_SET(WT_EVICT_CALL_NO_SPLIT);

    /* As re-entry into eviction is possible, only clear the statistics on the first entry. */
    if (__wt_session_gen((session), (WT_GEN_EVICT)) == 0) {
        WT_CLEAR(session->evict_timeline);
        session->evict_timeline.evict_start = __wt_clock(session);
    } else {
        session->evict_timeline.reentry_hs_eviction = true;
        session->evict_timeline.reentry_hs_evict_start = __wt_clock(session);
    }

    /*
     * Enter the eviction and split generation. If we re-enter eviction, leave the previous
     * generation (eviction or split) generation (which must be as low as the current generation),
     * untouched.
     */
    WT_ENTER_GENERATION(session, WT_GEN_EVICT);
    WT_ENTER_GENERATION(session, WT_GEN_SPLIT);

    /*
     * Immediately increment the forcible eviction counter, we might do an in-memory split and not
     * an eviction, which skips the other statistics.
     */
    if (LF_ISSET(WT_EVICT_CALL_URGENT)) {
        FLD_SET(stats_flags, WT_EVICT_STATS_URGENT);
        WT_STAT_CONN_INCR(session, eviction_force);

        /*
         * Track history store pages being force evicted while holding a history store cursor open.
         */
        if (session->hs_cursor_counter > 0 && WT_IS_HS(session->dhandle)) {
            FLD_SET(stats_flags, WT_EVICT_STATS_FORCE_HS);
            WT_STAT_CONN_INCR(session, eviction_force_hs);
        }
    }

    /*
     * Get exclusive access to the page if our caller doesn't have the tree locked down.
     */
    if (!closing) {
        WT_ERR(__evict_exclusive(session, ref));

        /*
         * Now the page is locked, remove it from the LRU eviction queue. We have to do this before
         * freeing the page memory or otherwise touching the reference because eviction paths assume
         * a non-NULL reference on the queue is pointing at valid memory.
         */
        __wti_evict_list_clear_page(session, ref);
    }

    if (F_ISSET_ATOMIC_16(page, WT_PAGE_PREFETCH))
        WT_STAT_CONN_INCR(session, eviction_consider_prefetch);

    /*
     * Review the page for conditions that would block its eviction. If the check fails (for
     * example, we find a page with active children), quit. Make this check for clean pages, too:
     * while unlikely eviction would choose an internal page with children, it's not disallowed.
     */
    WT_ERR(__evict_review(session, ref, flags, &inmem_split));

    /*
     * If we decide to do an in-memory split. Do it now. If an in-memory split completes, the page
     * stays in memory and the tree is left in the desired state: avoid the usual cleanup.
     */
    if (inmem_split) {
        WT_ERR(__wt_split_insert(session, ref));
        goto done;
    }

    if (__wt_page_is_modified(page))
        is_dirty = true;

    /* No need to reconcile the page if it is from a dead tree or it is clean. */
    if (!tree_dead && is_dirty)
        WT_ERR(__evict_reconcile(session, ref, flags));

    /* After this spot, the only recoverable failure is EBUSY. */
    ebusy_only = true;

    /*
     * Check we are not evicting an accessible internal page with an active split generation. We
     * should be able to evict anything if we are closing the dhandle and when the dhandle is
     * already dead.
     */
    WT_ASSERT(session,
      closing || !F_ISSET(ref, WT_REF_FLAG_INTERNAL) ||
        F_ISSET(session->dhandle, WT_DHANDLE_DEAD) ||
        !__wt_gen_active(session, WT_GEN_SPLIT, page->pg_intl_split_gen));

    /* Count evictions of internal pages during normal operation. */
    if (!closing && F_ISSET(ref, WT_REF_FLAG_INTERNAL))
        WT_STAT_CONN_DSRC_INCR(session, cache_eviction_internal);

    /*
     * Track the largest page size seen at eviction, it tells us something about our ability to
     * force pages out before they're larger than the cache. We don't care about races, it's just a
     * statistic.
     */
    page_size = __wt_atomic_loadsize(&page->memory_footprint);
    if (page_size > __wt_atomic_load64(&conn->evict->evict_max_page_size))
        __wt_atomic_store64(&conn->evict->evict_max_page_size, page_size);

    /* Clean page */
    if (!is_dirty) {
        if (page_size > __wt_atomic_load64(&conn->evict->evict_max_clean_page_size_per_checkpoint))
            __wt_atomic_store64(&conn->evict->evict_max_clean_page_size_per_checkpoint, page_size);
    } else {
        /* Dirty page */
        if (page_size > __wt_atomic_load64(&conn->evict->evict_max_dirty_page_size_per_checkpoint))
            __wt_atomic_store64(&conn->evict->evict_max_dirty_page_size_per_checkpoint, page_size);
    }
    /* Check if the page has updates */
    if (page->modify != NULL) {
        if (page_size >
          __wt_atomic_load64(&conn->evict->evict_max_updates_page_size_per_checkpoint))
            __wt_atomic_store64(
              &conn->evict->evict_max_updates_page_size_per_checkpoint, page_size);
    }

    /* Figure out whether reconciliation was done on the page */
    if (__wt_page_evict_clean(page)) {
        clean_page = true;
        FLD_SET(stats_flags, WT_EVICT_STATS_CLEAN);
    }

    /* Update the reference and discard the page. */
    if (__wt_ref_is_root(ref))
        __wt_ref_out(session, ref);
    else if ((clean_page && !F_ISSET(conn, WT_CONN_IN_MEMORY)) || tree_dead)
        /*
         * Pages that belong to dead trees never write back to disk and can't support page splits.
         */
        WT_ERR(__evict_page_clean_update(session, ref, flags));
    else
        WT_ERR(__evict_page_dirty_update(session, ref, flags));

    /*
     * We have loaded the new disk image and updated the tree structure. We can no longer fail after
     * this point.
     */

    if (0) {
err:
        if (!closing)
            __evict_exclusive_clear(session, ref, previous_state);

        if (ebusy_only && ret != EBUSY)
            WT_RET_PANIC(session, ret, "eviction failed when only EBUSY is allowed");
    }

done:
    if (ret == 0)
        FLD_SET(stats_flags, WT_EVICT_STATS_SUCCESS);
    __evict_stats_update(session, stats_flags);

    /* Leave any local eviction generation. */
    WT_LEAVE_GENERATION(session, WT_GEN_SPLIT);
    WT_LEAVE_GENERATION(session, WT_GEN_EVICT);

    return (ret);
}

/*
 * __evict_delete_ref --
 *     Mark a page reference deleted and check if the parent can reverse split.
 */
static int
__evict_delete_ref(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t flags)
{
    WT_DECL_RET;
    WT_PAGE *parent;
    WT_PAGE_INDEX *pindex;
    uint32_t ndeleted;

    if (__wt_ref_is_root(ref))
        return (0);

    /*
     * Avoid doing reverse splits when closing the file, it is wasted work and some structures may
     * have already been freed.
     */
    if (!LF_ISSET(WT_EVICT_CALL_NO_SPLIT | WT_EVICT_CALL_CLOSING)) {
        parent = ref->home;
        WT_INTL_INDEX_GET(session, parent, pindex);
        ndeleted = __wt_atomic_addv32(&pindex->deleted_entries, 1);

        /*
         * If more than 10% of the parent references are deleted, try a reverse split. Don't bother
         * if there is a single deleted reference: the internal page is empty and we have to wait
         * for eviction to notice.
         *
         * This will consume the deleted ref (and eventually free it). If the reverse split can't
         * get the access it needs because something is busy, be sure that the page still ends up
         * marked deleted.
         *
         * Don't do it if we are a VLCS tree and the child we're deleting is the leftmost child. The
         * reverse split will automatically remove the page entirely, creating a namespace gap at
         * the beginning of the internal page, and that leaves search nowhere to go. Note that the
         * situation will be handled safely if another child gets deleted, or if eviction comes for
         * a visit.
         */
        if (ndeleted > pindex->entries / 10 && pindex->entries > 1) {
            if (S2BT(session)->type == BTREE_COL_VAR && ref == pindex->index[0])
                WT_STAT_CONN_DSRC_INCR(session, cache_reverse_splits_skipped_vlcs);
            else {
                if ((ret = __wt_split_reverse(session, ref)) == 0) {
                    WT_STAT_CONN_DSRC_INCR(session, cache_reverse_splits);
                    return (0);
                }
                WT_RET_BUSY_OK(ret);

                /*
                 * The child must be locked after a failed reverse split.
                 */
                WT_ASSERT(session, WT_REF_GET_STATE(ref) == WT_REF_LOCKED);
            }
        }
    }

    WT_REF_SET_STATE(ref, WT_REF_DELETED);
    return (0);
}

/*
 * __evict_page_clean_update --
 *     Update a clean page's reference on eviction.
 */
static int
__evict_page_clean_update(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t flags)
{
    WT_DECL_RET;
    bool instantiated;

    /*
     * We might discard an instantiated deleted page, because instantiated pages are not marked
     * dirty by default. Check this before discarding the modify structure in __wt_ref_out.
     */
    if (ref->page->modify != NULL && ref->page->modify->instantiated)
        instantiated = true;
    else {
        WT_ASSERT(session, ref->page_del == NULL);
        instantiated = false;
    }

    /*
     * Discard the page and update the reference structure. A leaf page without a disk address is a
     * deleted page that either was created empty and never written out, or had its on-disk page
     * discarded already after the deletion became globally visible. It is not immediately clear if
     * it's possible to get an internal page without a disk address here, but if one appears it can
     * be deleted. (Note that deleting an internal page implicitly turns it into a leaf.)
     *
     * A page with a disk address is now on disk, unless it was deleted and instantiated and then
     * evicted unmodified, in which case it is still deleted. In the latter case set the state back
     * to WT_REF_DELETED.
     */
    __wt_ref_out(session, ref);
    if (ref->addr == NULL) {
        WT_WITH_PAGE_INDEX(session, ret = __evict_delete_ref(session, ref, flags));
        WT_RET_BUSY_OK(ret);
    } else
        WT_REF_SET_STATE(ref, instantiated ? WT_REF_DELETED : WT_REF_DISK);

    return (0);
}

/*
 * __evict_page_dirty_update --
 *     Update a dirty page's reference on eviction.
 */
static int
__evict_page_dirty_update(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t evict_flags)
{
    WT_ADDR *addr;
    WT_DECL_RET;
    WT_MULTI multi;
    WT_PAGE_MODIFY *mod;
    bool closing;
    void *tmp;

    mod = ref->page->modify;
    closing = FLD_ISSET(evict_flags, WT_EVICT_CALL_CLOSING);

    WT_ASSERT(session, ref->addr == NULL);

    switch (mod->rec_result) {
    case WT_PM_REC_EMPTY:
        /*
         * Page is empty: Update the parent to reference a deleted page. Reconciliation left the
         * page "empty", so there's no older transaction in the system that might need to see an
         * earlier version of the page. There's no backing address, if we're forced to "read" into
         * that namespace, we instantiate a new page instead of trying to read from the backing
         * store.
         */
        __wt_ref_out(session, ref);
        WT_WITH_PAGE_INDEX(session, ret = __evict_delete_ref(session, ref, evict_flags));
        WT_RET_BUSY_OK(ret);
        break;
    case WT_PM_REC_MULTIBLOCK:
        /*
         * Multiple blocks: Either a split where we reconciled a page and it turned into a lot of
         * pages or an in-memory page that got too large, we forcibly evicted it, and there wasn't
         * anything to write.
         *
         * The latter is a special case of forced eviction. Imagine a thread updating a small set
         * keys on a leaf page. The page is too large or has too many deleted items, so we try and
         * evict it, but after reconciliation there's only a small amount of live data (so it's a
         * single page we can't split), and if there's an older reader somewhere, there's data on
         * the page we can't write (so the page can't be evicted). In that case, we end up here with
         * a single block that we can't write. Take advantage of the fact we have exclusive access
         * to the page and rewrite it in memory.
         */
        if (mod->mod_multi_entries == 1) {
            WT_ASSERT(session, closing == false);
            WT_RET(__wt_split_rewrite(session, ref, &mod->mod_multi[0]));
        } else
            WT_RET(__wt_split_multi(session, ref, closing));
        break;
    case WT_PM_REC_REPLACE:
        /* 1-for-1 page swap: Update the parent to reference the replacement page. */
        WT_ASSERT(session, mod->mod_replace.addr != NULL);
        WT_RET(__wt_calloc_one(session, &addr));
        *addr = mod->mod_replace;
        mod->mod_replace.addr = NULL;
        mod->mod_replace.size = 0;
        ref->addr = addr;

        /*
         * Eviction wants to keep this page if we have a disk image, re-instantiate the page in
         * memory, else discard the page.
         */
        if (mod->mod_disk_image == NULL) {
            __wt_page_modify_clear(session, ref->page);
            __wt_ref_out(session, ref);
            WT_REF_SET_STATE(ref, WT_REF_DISK);
        } else {
            /* The split code works with WT_MULTI structures, build one for the disk image. */
            memset(&multi, 0, sizeof(multi));
            multi.disk_image = mod->mod_disk_image;
            /*
             * Store the disk image to a temporary pointer in case we fail to rewrite the page and
             * we need to link the new disk image back to the old disk image.
             */
            tmp = mod->mod_disk_image;
            mod->mod_disk_image = NULL;
            ret = __wt_split_rewrite(session, ref, &multi);
            if (ret != 0) {
                mod->mod_disk_image = tmp;
                return (ret);
            }
        }

        break;
    default:
        return (__wt_illegal_value(session, mod->rec_result));
    }

    return (0);
}

/*
 * __evict_child_check --
 *     Review an internal page for active children.
 */
static int
__evict_child_check(WT_SESSION_IMPL *session, WT_REF *parent)
{
    WT_REF *child;
    bool busy, visible;

    busy = false;

    /*
     * There may be cursors in the tree walking the list of child pages. The parent is locked, so
     * all we care about is cursors already in the child pages, no thread can enter them. Any cursor
     * moving through the child pages must be hazard pointer coupling between pages, where the page
     * on which it currently has a hazard pointer must be in a state other than on-disk. Walk the
     * child list forward, then backward, to ensure we don't race with a cursor walking in the
     * opposite direction from our check.
     */
    WT_INTL_FOREACH_BEGIN (session, parent->page, child) {
        /* It isn't safe to evict if there is a child on the pre-fetch queue. */
        if (F_ISSET_ATOMIC_8(child, WT_REF_FLAG_PREFETCH)) {
            busy = true;
            break;
        }

        switch (WT_REF_GET_STATE(child)) {
        case WT_REF_DISK:    /* On-disk */
        case WT_REF_DELETED: /* On-disk, deleted */
            break;
        default:
            busy = true;
        }
        if (busy)
            break;
    }
    WT_INTL_FOREACH_END;

    if (busy)
        return (__wt_set_return(session, EBUSY));

    WT_INTL_FOREACH_REVERSE_BEGIN (session, parent->page, child) {
        switch (WT_REF_GET_STATE(child)) {
        case WT_REF_DISK:    /* On-disk */
        case WT_REF_DELETED: /* On-disk, deleted */
            break;
        default:
            return (__wt_set_return(session, EBUSY));
        }
    }
    WT_INTL_FOREACH_END;

    /*
     * It is always OK to evict pages from checkpoint cursor trees if they don't have children, and
     * visibility checks for pages found to be deleted in the checkpoint aren't needed (or correct
     * when done in eviction threads).
     */
    if (WT_READING_CHECKPOINT(session))
        return (0);

    /*
     * The fast check is done and there are no cursors in the child pages. Make sure the child
     * WT_REF structures pages can be discarded.
     */
    WT_INTL_FOREACH_BEGIN (session, parent->page, child) {

        switch (WT_REF_GET_STATE(child)) {
        case WT_REF_DISK: /* On-disk */
            break;
        case WT_REF_DELETED: /* On-disk, deleted */
                             /*
                              * If the child page was part of a truncate, transaction rollback might
                              * switch this page into its previous state at any time, so the delete
                              * must be resolved before the parent can be evicted.
                              *
                              * We have the internal page locked, which prevents a search from
                              * descending into it. However, a walk from an adjacent leaf page could
                              * attempt to hazard couple into a child page and free the page_del
                              * structure as we are examining it. Flip the state to locked to make
                              * this check safe: if that fails, we have raced with a read and should
                              * give up on evicting the parent.
                              */
            if (!WT_REF_CAS_STATE(session, child, WT_REF_DELETED, WT_REF_LOCKED))
                return (__wt_set_return(session, EBUSY));
            /*
             * Insert a read/acquire barrier so we're guaranteed the page_del state we read below
             * comes after the locking operation on the ref state and therefore after the previous
             * unlock of the ref. Otherwise we might read an inconsistent view of the page deletion
             * info, and while many combinations are harmless and would just lead us to falsely
             * refuse to evict, some (e.g. reading committed as true and a stale durable timestamp
             * from before it was set by commit) are not.
             *
             * Note that while ordinarily a lock acquire should have an acquire (read/any) barrier
             * after it, because we are only reading the write part is irrelevant and a read/read
             * barrier is sufficient.
             *
             * FIXME-WT-9780: this and the CAS should be rolled into a WT_REF_TRYLOCK macro.
             */
            WT_ACQUIRE_BARRIER();

            /*
             * We can evict any truncation that's committed. However, restrictions in reconciliation
             * mean that it needs to be visible to us when we get there. And unfortunately we are
             * upstream of the point where eviction threads get snapshots. Plus, application threads
             * doing eviction can see their own uncommitted truncations. So, use the following
             * logic:
             *     1. First check if the operation is committed. If not, it's not visible for these
             *        purposes.
             *     2. If we already have a snapshot, use it to check visibility.
             *     3. If we do not but we're an eviction thread, go ahead. We will get a snapshot
             *        shortly and any committed operation will be visible in it.
             *     4. Otherwise, check if the operation is globally visible.
             *
             * Even though we specifically can't evict prepared truncations, we don't need to deploy
             * the special-case logic for prepared transactions in __wt_page_del_visible; prepared
             * transactions aren't committed so they'll fail the first check.
             */
            if (!__wt_page_del_committed_set(child->page_del))
                visible = false;
            else if (F_ISSET(session->txn, WT_TXN_HAS_SNAPSHOT))
                visible = __wt_page_del_visible(session, child->page_del, false);
            else if (F_ISSET(session, WT_SESSION_EVICTION))
                visible = true;
            else
                visible = __wt_page_del_visible_all(session, child->page_del, false);
            /* FIXME-WT-9780: is there a reason this doesn't use WT_REF_UNLOCK? */
            WT_REF_SET_STATE(child, WT_REF_DELETED);
            if (!visible)
                return (__wt_set_return(session, EBUSY));
            break;
        default:
            return (__wt_set_return(session, EBUSY));
        }
    }
    WT_INTL_FOREACH_END;

    return (0);
}

/*
 * __evict_review_obsolete_time_window --
 *     Check whether the ref has obsolete time window information and mark it for dirty eviction to
 *     remove those obsolete data. An exclusive lock on the page has already been obtained by the
 *     caller.
 */
static int
__evict_review_obsolete_time_window(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_ADDR_COPY addr;
    WT_BTREE *btree;
    WT_CONNECTION_IMPL *conn;
    WT_MULTI *multi;
    WT_PAGE_MODIFY *mod;
    WT_TIME_AGGREGATE newest_ta;
    uint32_t i;
    char time_string[WT_TIME_STRING_SIZE];

    btree = S2BT(session);
    conn = S2C(session);

    /* Too many pages have been cleaned for this btree. */
    if (__wt_atomic_load32(&btree->eviction_obsolete_tw_pages) >=
      conn->heuristic_controls.eviction_obsolete_tw_pages_dirty_max)
        return (0);

    /*
     * Pages that the application threads are evicting should not be included. Reconciliation must
     * be performed when converting a clean page to a dirty page, which can increase latency. This
     * check is bypassed if the session is configured with a debug option to evict the page when it
     * is released and no longer needed.
     */
    if (!F_ISSET(session, WT_SESSION_EVICTION) && !F_ISSET(session, WT_SESSION_DEBUG_RELEASE_EVICT))
        return (0);

    /* Do not perform any obsolete time window cleanup during the startup or shutdown phase. */
    if (F_ISSET(conn, WT_CONN_RECOVERING | WT_CONN_CLOSING_CHECKPOINT))
        return (0);

    /* If the file is being checkpointed, other threads can't evict dirty pages. */
    if (__wt_btree_syncing_by_other_session(session))
        return (0);

    /* The checkpoint cursor dhandle is read-only. Do not mark these pages as dirty. */
    if (WT_READING_CHECKPOINT(session))
        return (0);

    /*
     * Rewriting internal pages doesn't clean the obsolete time window until the leaf pages are
     * cleared from the obsolete time window.
     */
    WT_ASSERT(session, ref->page != NULL);
    if (WT_PAGE_IS_INTERNAL(ref->page))
        return (0);

    /* We are only interested in clean pages. */
    if (__wt_page_is_modified(ref->page))
        return (0);

    /* Limit the number of btrees that can be cleaned up. */
    if (__wt_atomic_load32(&btree->eviction_obsolete_tw_pages) == 0 &&
      __wt_atomic_load32(&btree->checkpoint_cleanup_obsolete_tw_pages) == 0 &&
      __wt_atomic_load32(&conn->heuristic_controls.obsolete_tw_btree_count) >=
        conn->heuristic_controls.obsolete_tw_btree_max)
        return (0);

    /* Don't add more cache pressure. */
    if (__wt_evict_needed(session, false, false, NULL) || __wt_evict_cache_stuck(session))
        return (0);

    /*
     * Initialize the time aggregate via the merge initialization, so that stop visibility is copied
     * across correctly. That is why we need the stop timestamp/transaction IDs to start as none,
     * otherwise we'd never mark anything as obsolete.
     */
    WT_TIME_AGGREGATE_INIT_MERGE(&newest_ta);

    mod = ref->page->modify;
    if (mod != NULL && mod->rec_result == WT_PM_REC_MULTIBLOCK) {
        /* Calculate the max stop time point by traversing all multi addresses. */
        for (multi = mod->mod_multi, i = 0; i < mod->mod_multi_entries; ++multi, ++i)
            WT_TIME_AGGREGATE_MERGE(session, &newest_ta, &multi->addr.ta);
    } else if (mod != NULL && mod->rec_result == WT_PM_REC_REPLACE)
        WT_TIME_AGGREGATE_COPY(&newest_ta, &mod->mod_replace.ta);
    else if (__wt_ref_addr_copy(session, ref, &addr))
        WT_TIME_AGGREGATE_COPY(&newest_ta, &addr.ta);

    /* The pages that are removed are eliminated during the checkpoint cleanup procedure. */
    if (WT_TIME_AGGREGATE_HAS_STOP(&newest_ta))
        return (0);

    /*
     * Mark the page as dirty to allow the page reconciliation to remove all information related to
     * an obsolete time window.
     */
    if (__wt_txn_has_newest_and_visible_all(session, newest_ta.newest_txn,
          WT_MAX(newest_ta.newest_start_durable_ts, newest_ta.newest_stop_durable_ts))) {
        __wt_verbose_debug2(session, WT_VERB_EVICTION,
          "%p in-memory page obsolete time window: time aggregate %s", (void *)ref,
          __wt_time_aggregate_to_string(&newest_ta, time_string));

        WT_RET(__wt_page_modify_init(session, ref->page));
        __wt_page_modify_set(session, ref->page);

        /*
         * Save that another tree has been processed if that's the first time it gets cleaned and
         * update the number of pages made dirty for that tree.
         */
        if (__wt_atomic_load32(&btree->eviction_obsolete_tw_pages) == 0 &&
          __wt_atomic_load32(&btree->checkpoint_cleanup_obsolete_tw_pages) == 0)
            __wt_atomic_addv32(&conn->heuristic_controls.obsolete_tw_btree_count, 1);
        __wt_atomic_addv32(&btree->eviction_obsolete_tw_pages, 1);
        WT_STAT_CONN_DSRC_INCR(session, cache_eviction_dirty_obsolete_tw);
    }

    return (0);
}

/*
 * __evict_review --
 *     Review the page and its subtree for conditions that would block its eviction.
 */
static int
__evict_review(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t evict_flags, bool *inmem_splitp)
{
    WT_BTREE *btree;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_PAGE *page;
    bool closing, modified;

    *inmem_splitp = false;

    btree = S2BT(session);
    conn = S2C(session);
    page = ref->page;
    closing = FLD_ISSET(evict_flags, WT_EVICT_CALL_CLOSING);

    /*
     * Fail if an internal has active children, the children must be evicted first. The test is
     * necessary but shouldn't fire much: the eviction code is biased for leaf pages, an internal
     * page shouldn't be selected for eviction until all children have been evicted.
     */
    if (F_ISSET(ref, WT_REF_FLAG_INTERNAL)) {
        WT_WITH_PAGE_INDEX(session, ret = __evict_child_check(session, ref));
        if (ret != 0)
            WT_STAT_CONN_INCR(session, eviction_fail_active_children_on_an_internal_page);
        WT_RET(ret);
    }

    /* It is always OK to evict pages from dead trees if they don't have children. */
    if (F_ISSET(session->dhandle, WT_DHANDLE_DEAD))
        return (0);

    /* Review the obsolete time window information before eviction. */
    WT_RET(__evict_review_obsolete_time_window(session, ref));

    /*
     * Retrieve the modified state of the page. This must happen after the check for evictable
     * internal pages otherwise there is a race where a page could be marked modified due to a child
     * being transitioned to WT_REF_DISK after the modified check and before we visited the ref
     * while walking the parent index.
     */
    modified = __wt_page_is_modified(page);

    /*
     * Clean pages can't be evicted when running in memory only. This should be uncommon - we don't
     * add clean pages to the queue.
     */
    if (F_ISSET(conn, WT_CONN_IN_MEMORY) && !modified && !closing)
        return (__wt_set_return(session, EBUSY));

    /* Check if the page can be evicted. */
    if (!closing) {
        /*
         * Update the oldest ID to avoid wasted effort should it have fallen behind current.
         */
        if (modified)
            WT_RET(__wt_txn_update_oldest(session, WT_TXN_OLDEST_STRICT));

        if (!__wt_page_can_evict(session, ref, inmem_splitp))
            return (__wt_set_return(session, EBUSY));

        /* Check for an append-only workload needing an in-memory split. */
        if (*inmem_splitp)
            return (0);
    }

    /* If the page is clean, we're done and we can evict. */
    if (!modified)
        return (0);

    /*
     * If we are trying to evict a dirty page that does not belong to history store(HS) and
     * checkpoint is processing the HS file, avoid evicting the dirty non-HS page for now if the
     * cache is already dominated by dirty HS content.
     *
     * Evicting an non-HS dirty page can generate even more HS content. As we cannot evict HS pages
     * while checkpoint is operating on the HS file, we can end up in a situation where we exceed
     * the cache size limit.
     */
    if (conn->txn_global.checkpoint_running_hs && !WT_IS_HS(btree->dhandle) &&
      __wti_evict_hs_dirty(session) && __wt_cache_full(session)) {
        WT_STAT_CONN_INCR(session, cache_eviction_blocked_checkpoint_hs);
        return (__wt_set_return(session, EBUSY));
    }
    /*
     * If reconciliation is disabled for this thread (e.g., during an eviction that writes to the
     * history store or reading a checkpoint), give up.
     */
    if (F_ISSET(session, WT_SESSION_NO_RECONCILE))
        return (__wt_set_return(session, EBUSY));

    return (0);
}

/*
 * __evict_reconcile --
 *     Reconcile the page for eviction.
 */
static int
__evict_reconcile(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t evict_flags)
{
    WT_BTREE *btree;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_EVICT *evict;
    uint32_t flags;
    bool closing, is_application_thread_snapshot_refreshed, is_eviction_thread,
      use_snapshot_for_app_thread;

    btree = S2BT(session);
    conn = S2C(session);
    flags = WT_REC_EVICT;
    closing = FLD_ISSET(evict_flags, WT_EVICT_CALL_CLOSING);

    evict = conn->evict;
    is_application_thread_snapshot_refreshed = false;

    /*
     * Urgent eviction and forced eviction want two different behaviors for inefficient update
     * restore evictions, pass this flag so that reconciliation knows which to use.
     */
    if (FLD_ISSET(evict_flags, WT_EVICT_CALL_URGENT))
        LF_SET(WT_REC_CALL_URGENT);

    /*
     * If we have an exclusive lock (we're discarding the tree), assert there are no updates we
     * cannot read.
     */
    if (closing)
        LF_SET(WT_REC_EVICT_CALL_CLOSING | WT_REC_VISIBILITY_ERR);
    /*
     * Don't set any other flags for internal pages: there are no update lists to be saved and
     * restored, changes can't be written into the history store table, nor can we re-create
     * internal pages in memory.
     *
     * Don't set any other flags for history store table as all the content is evictable.
     */
    else if (F_ISSET(ref, WT_REF_FLAG_INTERNAL) || WT_IS_HS(btree->dhandle))
        ;
    /* Always do update restore for in-memory database. */
    else if (F_ISSET(conn, WT_CONN_IN_MEMORY))
        LF_SET(WT_REC_IN_MEMORY | WT_REC_SCRUB);
    /* For data store leaf pages, write the history to history store except for metadata. */
    else if (!WT_IS_METADATA(btree->dhandle)) {
        LF_SET(WT_REC_HS);

        /*
         * Scrub and we're supposed to or toss it in sometimes if we are in debugging mode.
         *
         * Note that don't scrub if checkpoint is running on the tree.
         */
        if (!WT_SESSION_BTREE_SYNC(session)) {
            bool can_scrub = (F_ISSET(evict, WT_EVICT_CACHE_SCRUB) ||
              (FLD_ISSET(conn->debug_flags, WT_CONN_DEBUG_EVICT_AGGRESSIVE_MODE) &&
                __wt_random(&session->rnd_random) % 3 == 0));

            /*
             * Scrub only if cache is under the clean eviction target or the page has high read
             * generation (the page is hot and we want to keep it in cache).
             */
            if (can_scrub &&
              (!__wt_evict_clean_needed(session, NULL) ||
                ref->page->read_gen > __evict_read_gen(session))) {
                LF_SET(WT_REC_SCRUB);
            }
        }
    }

    /*
     * Acquire a snapshot if coming through the eviction thread route. Also, if we have entered
     * eviction through application threads then we save the existing snapshot and refresh to
     * acquire a new snapshot, once the application threads are done with eviction then we switch
     * back the snapshot to its original. Avoid using snapshots when application transactions are in
     * the final stages of commit or rollback as they have already released the snapshot. Otherwise,
     * it becomes harder in the later part of the code to detect updates that belonged to the last
     * running application transaction.
     */
    use_snapshot_for_app_thread = !F_ISSET(session, WT_SESSION_INTERNAL) &&
      !WT_IS_METADATA(session->dhandle) &&
      __wt_atomic_loadv64(&WT_SESSION_TXN_SHARED(session)->id) != WT_TXN_NONE &&
      F_ISSET(session->txn, WT_TXN_HAS_SNAPSHOT);
    is_eviction_thread = F_ISSET(session, WT_SESSION_EVICTION);

    /* Make sure that both conditions above are not true at the same time. */
    WT_ASSERT(session, !use_snapshot_for_app_thread || !is_eviction_thread);

    /*
     * If checkpoint is running concurrently, set the checkpoint running flag and we will abort the
     * eviction if we detect any updates without timestamps.
     */
    if (__wt_atomic_loadvbool(&conn->txn_global.checkpoint_running))
        LF_SET(WT_REC_CHECKPOINT_RUNNING);

    /* Eviction thread doing eviction. */
    if (is_eviction_thread)
        /*
         * Eviction threads do not need to pin anything in the cache. We have an exclusive lock for
         * the page being evicted so we are sure that the page will always be there while it is
         * being processed. Therefore, we use snapshot API that doesn't publish shared IDs to the
         * outside world.
         */
        __wt_txn_bump_snapshot(session);
    else if (use_snapshot_for_app_thread) {
        /*
         * If we couldn't make progress with the application thread's existing snapshot, save the
         * existing snapshot and refresh to acquire a new one. Then try eviction again. Once the
         * application threads are done with eviction, the application thread's snapshot is switched
         * back to the original.
         */
        if (F_ISSET(session->txn, WT_TXN_REFRESH_SNAPSHOT)) {
            WT_RET(__wt_txn_snapshot_save_and_refresh(session));
            is_application_thread_snapshot_refreshed = true;
            WT_STAT_CONN_INCR(session, application_evict_snapshot_refreshed);
        }

        LF_SET(WT_REC_APP_EVICTION_SNAPSHOT);
    } else if (!WT_SESSION_BTREE_SYNC(session))
        LF_SET(WT_REC_VISIBLE_ALL);

    WT_ASSERT(session, LF_ISSET(WT_REC_VISIBLE_ALL) || F_ISSET(session->txn, WT_TXN_HAS_SNAPSHOT));

    /* We should not be trying to evict using a checkpoint-cursor transaction. */
    WT_ASSERT(session, !F_ISSET(session->txn, WT_TXN_IS_CHECKPOINT));

    /*
     * Reconcile the page. Force read-committed isolation level if we are using snapshots for
     * eviction workers or application threads.
     */
    if (is_eviction_thread || use_snapshot_for_app_thread)
        WT_WITH_TXN_ISOLATION(
          session, WT_ISO_READ_COMMITTED, ret = __wt_reconcile(session, ref, NULL, flags));
    else
        ret = __wt_reconcile(session, ref, NULL, flags);

    if (ret != 0)
        WT_STAT_CONN_INCR(session, eviction_fail_in_reconciliation);

    if (is_eviction_thread)
        __wt_txn_release_snapshot(session);
    else if (is_application_thread_snapshot_refreshed)
        __wt_txn_snapshot_release_and_restore(session);

    WT_RET(ret);

    /*
     * Success: assert that the page is clean or reconciliation was configured to save updates.
     */
    WT_ASSERT(session,
      !__wt_page_is_modified(ref->page) || LF_ISSET(WT_REC_HS | WT_REC_IN_MEMORY) ||
        WT_IS_METADATA(btree->dhandle));

    return (0);
}
