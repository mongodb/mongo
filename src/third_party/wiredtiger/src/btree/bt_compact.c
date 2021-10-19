/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __compact_rewrite --
 *     Return if a modified page needs to be re-written.
 */
static int
__compact_rewrite(WT_SESSION_IMPL *session, WT_REF *ref, bool *skipp)
{
    WT_ADDR_COPY addr;
    WT_BM *bm;
    WT_MULTI *multi;
    WT_PAGE_MODIFY *mod;
    uint32_t i;

    *skipp = true; /* Default skip. */

    bm = S2BT(session)->bm;

    /* If the page is clean, test the original addresses. */
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
        return (
          bm->compact_page_skip(bm, session, mod->mod_replace.addr, mod->mod_replace.size, skipp));

    if (mod->rec_result == WT_PM_REC_MULTIBLOCK)
        for (multi = mod->mod_multi, i = 0; i < mod->mod_multi_entries; ++multi, ++i) {
            if (multi->addr.addr == NULL)
                continue;
            WT_RET(bm->compact_page_skip(bm, session, multi->addr.addr, multi->addr.size, skipp));
            if (!*skipp)
                break;
        }

    return (0);
}

/*
 * __compact_rewrite_lock --
 *     Return if a page needs to be re-written.
 */
static int
__compact_rewrite_lock(WT_SESSION_IMPL *session, WT_REF *ref, bool *skipp)
{
    WT_BTREE *btree;
    WT_DECL_RET;

    btree = S2BT(session);

    /*
     * Reviewing in-memory pages requires looking at page reconciliation results, because we care
     * about where the page is stored now, not where the page was stored when we first read it into
     * the cache. We need to ensure we don't race with page reconciliation as it's writing the page
     * modify information. There are two ways we call reconciliation: checkpoints and eviction. We
     * are holding a hazard pointer that blocks eviction, but there's nothing blocking a checkpoint.
     * Get the tree's flush lock which blocks threads writing pages for checkpoints. If checkpoint
     * is holding the lock, quit working this file, we'll visit it again in our next pass.
     */
    WT_RET(__wt_spin_trylock(session, &btree->flush_lock));

    ret = __compact_rewrite(session, ref, skipp);

    /* Unblock threads writing leaf pages. */
    __wt_spin_unlock(session, &btree->flush_lock);

    return (ret);
}

/*
 * __compact_progress --
 *     Output a compact progress message.
 */
static void
__compact_progress(WT_SESSION_IMPL *session)
{
    struct timespec cur_time;
    WT_BM *bm;
    uint64_t time_diff;

    if (!WT_VERBOSE_ISSET(session, WT_VERB_COMPACT_PROGRESS))
        return;

    bm = S2BT(session)->bm;
    __wt_epoch(session, &cur_time);

    /* Log one progress message every twenty seconds. */
    time_diff = WT_TIMEDIFF_SEC(cur_time, session->compact->begin);
    if (time_diff / WT_PROGRESS_MSG_PERIOD > session->compact->prog_msg_count) {
        __wt_verbose(session, WT_VERB_COMPACT_PROGRESS,
          "Compact running for %" PRIu64 " seconds; reviewed %" PRIu64 " pages, skipped %" PRIu64
          " pages, wrote %" PRIu64 " pages",
          time_diff, bm->block->compact_pages_reviewed, bm->block->compact_pages_skipped,
          bm->block->compact_pages_written);
        session->compact->prog_msg_count++;
    }
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
    bool skip;

    bm = S2BT(session)->bm;
    ref = NULL;

    WT_STAT_DATA_INCR(session, session_compact);

    /*
     * Check if compaction might be useful -- the API layer will quit trying to compact the data
     * source if we make no progress, set a flag if the block layer thinks compaction is possible.
     */
    WT_RET(bm->compact_skip(bm, session, &skip));
    if (skip) {
        WT_STAT_CONN_INCR(session, session_table_compact_skipped);
        WT_STAT_DATA_INCR(session, btree_compact_skipped);
        return (0);
    }

    /* Walk the tree reviewing pages to see if they should be re-written. */
    for (i = 0;;) {

        /* Track progress. */
        WT_STAT_DATA_SET(session, btree_compact_pages_reviewed, bm->block->compact_pages_reviewed);
        WT_STAT_DATA_SET(session, btree_compact_pages_skipped, bm->block->compact_pages_skipped);
        WT_STAT_DATA_SET(
          session, btree_compact_pages_write_selected, bm->block->compact_pages_written);

        /*
         * Periodically check if we've timed out or eviction is stuck. Quit if eviction is stuck,
         * we're making the problem worse.
         */
        if (++i > 100) {
            __compact_progress(session);
            WT_ERR(__wt_session_compact_check_timeout(session));

            if (__wt_cache_stuck(session))
                WT_ERR(EBUSY);

            i = 0;
        }

        /*
         * Compact pulls pages into cache during the walk without checking whether the cache is
         * full. Check now to throttle compact to match eviction speed.
         */
        WT_ERR(__wt_cache_eviction_check(session, false, false, NULL));

        /*
         * Pages read for compaction aren't "useful"; don't update the read generation of pages
         * already in memory, and if a page is read, set its generation to a low value so it is
         * evicted quickly.
         */
        WT_ERR(__wt_tree_walk_custom_skip(
          session, &ref, __wt_compact_page_skip, NULL, WT_READ_NO_GEN | WT_READ_WONT_NEED));
        if (ref == NULL)
            break;

        /*
         * Cheap checks that don't require locking.
         *
         * Ignore the root: it may not have a replacement address, and besides, if anything else
         * gets written, so will it.
         *
         * Ignore dirty pages, checkpoint will likely write them. There are cases where checkpoint
         * can skip dirty pages: to avoid that, we could alter the transactional information of the
         * page, which is what checkpoint reviews to decide if a page can be skipped. Not doing that
         * for now, the repeated checkpoints that compaction requires are more than likely to pick
         * up all dirty pages at some point.
         */
        if (__wt_ref_is_root(ref))
            continue;
        if (__wt_page_is_modified(ref->page))
            continue;

        WT_ERR(__compact_rewrite_lock(session, ref, &skip));
        if (skip)
            continue;

        /* Rewrite the page: mark the page and tree dirty. */
        WT_ERR(__wt_page_modify_init(session, ref->page));
        __wt_page_modify_set(session, ref->page);

        session->compact_state = WT_COMPACT_SUCCESS;
        WT_STAT_DATA_INCR(session, btree_compact_pages_rewritten);
    }

err:
    if (ref != NULL)
        WT_TRET(__wt_page_release(session, ref, 0));

    return (ret);
}

/*
 * __wt_compact_page_skip --
 *     Return if compaction requires we read this page.
 */
int
__wt_compact_page_skip(WT_SESSION_IMPL *session, WT_REF *ref, void *context, bool *skipp)
{
    WT_ADDR_COPY addr;
    WT_BM *bm;
    uint8_t previous_state;
    bool diskaddr;

    WT_UNUSED(context);

    *skipp = false; /* Default to reading */

    /* Internal pages must be read to walk the tree. */
    if (F_ISSET(ref, WT_REF_FLAG_INTERNAL))
        return (0);

    /*
     * Skip deleted pages, rewriting them doesn't seem useful; in a better world we'd write the
     * parent to delete the page.
     */
    if (ref->state == WT_REF_DELETED) {
        *skipp = true;
        return (0);
    }

    /*
     * If the page is in-memory, we want to look at it (it may have been modified and written, and
     * the current location is the interesting one in terms of compaction, not the original).
     */
    if (ref->state != WT_REF_DISK)
        return (0);

    /*
     * Lock the WT_REF and if it's still on-disk, get a copy of the address. This is safe because
     * it's an on-disk page and we're holding the WT_REF locked, so nobody can read the page giving
     * either checkpoint or eviction a chance to modify the address.
     */
    WT_REF_LOCK(session, ref, &previous_state);
    diskaddr = previous_state == WT_REF_DISK && __wt_ref_addr_copy(session, ref, &addr);
    WT_REF_UNLOCK(ref, previous_state);
    if (!diskaddr)
        return (0);

    /* Ask the block-manager if it's useful to rewrite the page. */
    bm = S2BT(session)->bm;
    return (bm->compact_page_skip(bm, session, addr.addr, addr.size, skipp));
}
