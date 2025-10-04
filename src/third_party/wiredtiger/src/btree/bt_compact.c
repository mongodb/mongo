/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __compact_page_inmem_check_addrs --
 *     Return if a clean, in-memory page needs to be re-written.
 */
static int
__compact_page_inmem_check_addrs(WT_SESSION_IMPL *session, WT_REF *ref, bool *skipp)
{
    WT_ADDR_COPY addr;
    WT_BM *bm;
    WT_MULTI *multi;
    WT_PAGE_MODIFY *mod;
    uint32_t i;

    WT_ASSERT_SPINLOCK_OWNED(session, &S2BT(session)->flush_lock);

    *skipp = true; /* Default skip. */

    bm = S2BT(session)->bm;

    /* If the page is currently clean, test the original addresses. */
    if (__wt_page_evict_clean(ref->page))
        return (__wt_ref_addr_copy(session, ref, &addr) ?
            bm->compact_page_skip(bm, session, addr.addr, addr.size, skipp) :
            0);

    /*
     * If the page is a replacement, test the replacement addresses. Ignore empty pages, they get
     * merged into the parent.
     */
    mod = ref->page->modify;
    if (mod->rec_result == WT_PM_REC_REPLACE)
        return (bm->compact_page_skip(
          bm, session, mod->mod_replace.block_cookie, mod->mod_replace.block_cookie_size, skipp));

    if (mod->rec_result == WT_PM_REC_MULTIBLOCK)
        for (multi = mod->mod_multi, i = 0; i < mod->mod_multi_entries; ++multi, ++i) {
            if (multi->addr.block_cookie == NULL)
                continue;
            WT_RET(bm->compact_page_skip(
              bm, session, multi->addr.block_cookie, multi->addr.block_cookie_size, skipp));
            if (!*skipp)
                break;
        }

    return (0);
}

/*
 * __compact_page_inmem --
 *     Return if an in-memory page needs to be re-written.
 */
static int
__compact_page_inmem(WT_SESSION_IMPL *session, WT_REF *ref, bool *skipp)
{
    *skipp = true; /* Default skip. */

    WT_ASSERT_SPINLOCK_OWNED(session, &S2BT(session)->flush_lock);

    /*
     * Ignore dirty pages, checkpoint will likely write them. There are cases where checkpoint can
     * skip dirty pages: to avoid that, we could alter the transactional information of the page,
     * which is what checkpoint reviews to decide if a page can be skipped. Not doing that for now,
     * the repeated checkpoints that compaction requires are more than likely to pick up all dirty
     * pages at some point.
     *
     * Check clean page addresses, and mark page and tree dirty if the page needs to be rewritten.
     */
    if (__wt_page_is_modified(ref->page))
        *skipp = false;
    else {
        WT_RET(__compact_page_inmem_check_addrs(session, ref, skipp));

        if (!*skipp) {
            WT_RET(__wt_page_modify_init(session, ref->page));
            __wt_page_modify_set(session, ref->page);
        }
    }

    /* If rewriting the page, have reconciliation write new blocks. */
    if (!*skipp)
        F_SET_ATOMIC_16(ref->page, WT_PAGE_COMPACTION_WRITE);

    return (0);
}

/*
 * __compact_page_replace_addr --
 *     Replace a page's WT_ADDR.
 */
static int
__compact_page_replace_addr(WT_SESSION_IMPL *session, WT_REF *ref, WT_ADDR_COPY *copy)
{
    WT_ADDR *addr;
    WT_CELL_UNPACK_ADDR unpack;
    WT_DECL_RET;

    WT_ASSERT_SPINLOCK_OWNED(session, &S2BT(session)->flush_lock);

    /*
     * If there's no address at all (the page has never been written), allocate a new WT_ADDR
     * structure, otherwise, the address has already been instantiated, replace the cookie.
     */
    addr = ref->addr;
    WT_ASSERT(session, addr != NULL);

    if (__wt_off_page(ref->home, addr))
        __wti_ref_addr_safe_free(session, addr->block_cookie, addr->block_cookie_size);
    else {
        __wt_cell_unpack_addr(session, ref->home->dsk, (WT_CELL *)addr, &unpack);

        WT_RET(__wt_calloc_one(session, &addr));
        addr->ta.newest_start_durable_ts = unpack.ta.newest_start_durable_ts;
        addr->ta.newest_stop_durable_ts = unpack.ta.newest_stop_durable_ts;
        addr->ta.oldest_start_ts = unpack.ta.oldest_start_ts;
        addr->ta.newest_txn = unpack.ta.newest_txn;
        addr->ta.newest_stop_ts = unpack.ta.newest_stop_ts;
        addr->ta.newest_stop_txn = unpack.ta.newest_stop_txn;
        switch (unpack.raw) {
        case WT_CELL_ADDR_DEL:
            addr->type = WT_ADDR_LEAF_NO;
            break;
        case WT_CELL_ADDR_INT:
            addr->type = WT_ADDR_INT;
            break;
        case WT_CELL_ADDR_LEAF:
            addr->type = WT_ADDR_LEAF;
            break;
        case WT_CELL_ADDR_LEAF_NO:
            addr->type = WT_ADDR_LEAF_NO;
            break;
        }
    }

    WT_ERR(__wt_strndup(session, copy->addr, copy->size, &addr->block_cookie));
    addr->block_cookie_size = copy->size;

    ref->addr = addr;
    return (0);

err:
    if (addr != ref->addr)
        __wt_free(session, addr);
    return (ret);
}

/*
 * __compact_page --
 *     Compaction for a single page.
 */
static int
__compact_page(WT_SESSION_IMPL *session, WT_REF *ref, bool *skipp)
{
    WT_ADDR_COPY copy;
    WT_BM *bm;
    WT_DECL_RET;
    WT_REF_STATE previous_state;
    size_t addr_size;

    *skipp = true; /* Default skip. */

    WT_ASSERT_SPINLOCK_OWNED(session, &S2BT(session)->flush_lock);

    /* Lock the WT_REF. */
    WT_REF_LOCK(session, ref, &previous_state);

    /*
     * Don't bother rewriting deleted pages but also don't skip. The on-disk block with an address
     * is discarded by the next checkpoint, if it has not already been freed.
     */
    if (previous_state == WT_REF_DELETED && ref->page_del == NULL && ref->addr != NULL)
        *skipp = false;

    /*
     * If it's on disk, get a copy of the address and ask the block manager to rewrite the block if
     * it's useful. This is safe because we're holding the WT_REF locked, so nobody can read the
     * page giving eviction a chance to modify the address. Note that a deleted ref that is not
     * globally visible is still on disk.
     *
     * In this path, we are holding the WT_REF lock across two OS buffer cache I/Os (the read of the
     * original block and the write of the new block), plus whatever overhead that entails. It's not
     * ideal, we could release the lock, but then we'd have to deal with the block having been read
     * into memory while we were moving it.
     */
    if ((previous_state == WT_REF_DISK ||
          (previous_state == WT_REF_DELETED && ref->page_del != NULL)) &&
      __wt_ref_addr_copy(session, ref, &copy)) {
        bm = S2BT(session)->bm;
        addr_size = copy.size;
        WT_ERR(bm->compact_page_rewrite(bm, session, copy.addr, &addr_size, skipp));
        if (!*skipp) {
            copy.size = (uint8_t)addr_size;
            WT_ERR(__compact_page_replace_addr(session, ref, &copy));
        }
    }

    /*
     * Ignore pages that aren't in-memory for some reason other than they're on-disk, for example,
     * they might have split or been deleted while we were locking the WT_REF. This includes the
     * case where we found an on-disk page and either rewrite the block successfully or failed to
     * get a copy of the address (which shouldn't ever happen, but if that goes wrong, it's not our
     * problem to solve).
     *
     * In this path, we are holding the WT_REF lock across some in-memory checks and possibly one or
     * more calls to the underlying block manager which is going to search the list of extents to
     * figure out if the block is worth rewriting. It's not ideal because we're blocking the
     * application's worker threads: we could release the lock, but then we'd have to acquire a
     * hazard pointer to ensure eviction didn't select the page.
     */
    if (previous_state == WT_REF_MEM)
        WT_ERR(__compact_page_inmem(session, ref, skipp));

err:
    WT_REF_UNLOCK(ref, previous_state);

    return (ret);
}

/*
 * __compact_walk_internal --
 *     Walk an internal page for compaction.
 */
static int
__compact_walk_internal(WT_SESSION_IMPL *session, WT_REF *parent)
{
    WT_DECL_RET;
    WT_REF *ref;
    bool overall_progress, skipp;

    WT_ASSERT(session, F_ISSET(parent, WT_REF_FLAG_INTERNAL));

    ref = NULL; /* [-Wconditional-uninitialized] */

    /*
     * We could corrupt a checkpoint if we moved a block that's part of the checkpoint, that is, if
     * we race with checkpoint's review of the tree. Get the tree's flush lock which blocks threads
     * writing pages for checkpoints, and hold it long enough to review a single internal page. If
     * checkpoint has the lock, check for a compact interrupt and cache stuck before trying again.
     */
    while ((ret = __wt_spin_trylock(session, &S2BT(session)->flush_lock)) == EBUSY) {
        WT_STAT_CONN_INCR(session, session_table_compact_conflicting_checkpoint);

        __wt_verbose_level(session, WT_VERB_COMPACT, WT_VERBOSE_DEBUG_1,
          "The compaction of the data handle %s returned EBUSY due to an in-progress "
          "conflicting checkpoint. Compaction of this data handle will resume after checkpoint "
          "completes.",
          session->dhandle->name);

        WT_RET(__wt_session_compact_check_interrupted(session));

        if (__wt_evict_cache_stuck(session))
            WT_RET(EBUSY);

        __wt_sleep(1, 0);
    }

    WT_ERR(ret);

    /*
     * Walk the internal page and check any leaf pages it references; skip internal pages, we'll
     * visit them individually.
     */
    overall_progress = false;
    WT_INTL_FOREACH_BEGIN (session, parent->page, ref) {
        if (F_ISSET(ref, WT_REF_FLAG_LEAF)) {
            WT_ERR(__compact_page(session, ref, &skipp));
            if (!skipp)
                overall_progress = true;
        }
    }
    WT_INTL_FOREACH_END;

    /*
     * If we moved a leaf page, we'll write the parent. If we didn't move a leaf page, check pages
     * other than the root to see if we want to move the internal page itself. (Skip the root as a
     * forced checkpoint will always rewrite it, and you can't just "move" a root page.)
     */
    if (!overall_progress && !__wt_ref_is_root(parent)) {
        WT_ERR(__compact_page(session, parent, &skipp));
        if (!skipp)
            overall_progress = true;
    }

    /* If we found a page to compact, mark the parent and tree dirty and report success. */
    if (overall_progress) {
        WT_ERR(__wt_page_parent_modify_set(session, ref, false));
        session->compact_state = WT_COMPACT_SUCCESS;
    }

err:
    /* Unblock checkpoint threads. */
    __wt_spin_unlock(session, &S2BT(session)->flush_lock);

    return (ret);
}

/*
 * __compact_walk_page_skip --
 *     Skip leaf pages, all we want are internal pages.
 */
static int
__compact_walk_page_skip(
  WT_SESSION_IMPL *session, WT_REF *ref, void *context, bool visible_all, bool *skipp)
{
    WT_UNUSED(context);
    WT_UNUSED(session);
    WT_UNUSED(visible_all);

    /* All we want are the internal pages. */
    *skipp = F_ISSET(ref, WT_REF_FLAG_LEAF) ? true : false;
    return (0);
}

/*
 * __wt_compact --
 *     Compact a file.
 */
int
__wt_compact(WT_SESSION_IMPL *session)
{
    WT_BM *bm;
    WT_DECL_RET;
    WT_REF *ref;
    u_int i;
    bool eviction_happened, first, skip;

    uint64_t stats_pages_reviewed; /* Pages reviewed */

    bm = S2BT(session)->bm;
    ref = NULL;

    WT_STAT_DSRC_INCR(session, session_compact);

    /*
     * Check if compaction might be useful (the API layer will quit trying to compact the data
     * source if we make no progress).
     */
    WT_RET(bm->compact_skip(bm, session, &skip));
    if (skip) {
        WT_STAT_CONN_INCR(session, session_table_compact_skipped);
        WT_STAT_DSRC_INCR(session, btree_compact_skipped);

        /*
         * Print the "skipping compaction" message only if this is the first time we are working on
         * this table.
         */
        __wt_block_compact_get_progress_stats(session, bm, &stats_pages_reviewed);
        if (stats_pages_reviewed == 0)
            __wt_verbose_info(session, WT_VERB_COMPACT,
              "%s: there is no useful work to do - skipping compaction", bm->block->name);

        return (0);
    }

    /* Walk the tree reviewing pages to see if they should be re-written. */
    first = true;
    for (i = 0;;) {

        /*
         * Periodically check if compaction has been interrupted or if eviction is stuck, quit if
         * this is the case.
         */
        if (first || ++i > 100) {
            if (!first)
                bm->compact_progress(bm, session);
            WT_ERR(__wt_session_compact_check_interrupted(session));

            if (__wt_evict_cache_stuck(session))
                WT_ERR(EBUSY);

            first = false;
            i = 0;
        }

        /*
         * Compact pulls pages into cache during the walk without checking whether the cache is
         * full. Check now to throttle compact to match eviction speed.
         */
        WT_ERR(
          __wt_evict_app_assist_worker_check(session, false, false, false, &eviction_happened));
        if (eviction_happened)
            WT_STAT_CONN_INCR(session, session_table_compact_eviction);

        /*
         * Pages read for compaction aren't "useful"; don't update the read generation of pages
         * already in memory, and if a page is read, set its generation to a low value so it is
         * evicted quickly.
         */
        WT_ERR(__wt_tree_walk_custom_skip(session, &ref, __compact_walk_page_skip, NULL,
          WT_READ_INTERNAL_OP | WT_READ_VISIBLE_ALL | WT_READ_WONT_NEED));
        if (ref == NULL)
            break;

        /*
         * The compact walk only flags internal pages for review, but there is a rare case where an
         * WT_REF in the WT_REF_DISK state pointing to an internal page, can transition to a leaf
         * page when it is being read in. Handle that here, by re-checking the page type now that
         * the page is in memory.
         */
        if (F_ISSET(ref, WT_REF_FLAG_INTERNAL)) {
            bm->block->compact_internal_pages_reviewed++;
            WT_WITH_PAGE_INDEX(session, ret = __compact_walk_internal(session, ref));
        }

        /* Track progress. */
        __wt_block_compact_get_progress_stats(session, bm, &stats_pages_reviewed);

        WT_ERR(ret);
    }

err:
    WT_TRET(__wt_page_release(session, ref, 0));

    return (ret);
}
