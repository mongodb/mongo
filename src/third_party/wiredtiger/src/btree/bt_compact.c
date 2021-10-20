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
 *     Return if a page needs to be re-written.
 */
static int
__compact_rewrite(WT_SESSION_IMPL *session, WT_REF *ref, bool *skipp)
{
    WT_BM *bm;
    WT_MULTI *multi;
    WT_PAGE *page;
    WT_PAGE_MODIFY *mod;
    size_t addr_size;
    uint32_t i;
    const uint8_t *addr;

    *skipp = true; /* Default skip. */

    bm = S2BT(session)->bm;
    page = ref->page;

    /* If the page is clean, test the original addresses. */
    if (__wt_page_evict_clean(page)) {
        __wt_ref_info(ref, &addr, &addr_size, NULL);
        if (addr == NULL)
            return (0);
        return (bm->compact_page_skip(bm, session, addr, addr_size, skipp));
    }

    /*
     * If the page is a replacement, test the replacement addresses.
     * Ignore empty pages, they get merged into the parent.
     *
     * Page-modify variable initialization done here because the page could
     * be modified while we're looking at it, so the page modified structure
     * may appear at any time (but cannot disappear). We've confirmed there
     * is a page modify structure, it's OK to look at it.
     */
    mod = page->modify;
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
 *     Lock out checkpoints and return if a page needs to be re-written.
 */
static int
__compact_rewrite_lock(WT_SESSION_IMPL *session, WT_REF *ref, bool *skipp)
{
    WT_BTREE *btree;
    WT_DECL_RET;

    *skipp = true; /* Default skip. */

    btree = S2BT(session);

    /*
     * Reviewing in-memory pages requires looking at page reconciliation
     * results, because we care about where the page is stored now, not
     * where the page was stored when we first read it into the cache.
     * We need to ensure we don't race with page reconciliation as it's
     * writing the page modify information.
     *
     * There are two ways we call reconciliation: checkpoints and eviction.
     * Get the tree's flush lock which blocks threads writing pages for
     * checkpoints. If checkpoint is holding the lock, quit working this
     * file, we'll visit it again in our next pass. We don't have to worry
     * about eviction, we're holding a hazard pointer on the WT_REF, it's
     * not going anywhere.
     */
    WT_RET(__wt_spin_trylock(session, &btree->flush_lock));

    ret = __compact_rewrite(session, ref, skipp);

    /* Unblock threads writing leaf pages. */
    __wt_spin_unlock(session, &btree->flush_lock);

    return (ret);
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
    if (skip)
        return (0);

    /* Walk the tree reviewing pages to see if they should be re-written. */
    for (i = 0;;) {
        /*
         * Periodically check if we've timed out or eviction is stuck. Quit if eviction is stuck,
         * we're making the problem worse.
         */
        if (++i > 100) {
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
         * Ignore the root: it may not have a replacement address, and
         * besides, if anything else gets written, so will it.
         *
         * Ignore dirty pages, checkpoint writes them regardless.
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
        WT_STAT_DATA_INCR(session, btree_compact_rewrite);
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
    WT_BM *bm;
    WT_DECL_RET;
    size_t addr_size;
    const uint8_t *addr;
    u_int type;

    WT_UNUSED(context);
    *skipp = false; /* Default to reading */

    /*
     * Skip deleted pages, rewriting them doesn't seem useful; in a better world we'd write the
     * parent to delete the page.
     */
    if (ref->state == WT_REF_DELETED) {
        *skipp = true;
        return (0);
    }

    /*
     * If the page is in-memory, we want to look at it (it may have been
     * modified and written, and the current location is the interesting
     * one in terms of compaction, not the original location).
     *
     * This test could be combined with the next one, but this is a cheap
     * test and the next one is expensive.
     */
    if (ref->state != WT_REF_DISK)
        return (0);

    /*
     * There's nothing to prevent the WT_REF state from changing underfoot, which can change its
     * address. For example, the WT_REF address might reference an on-page cell, and page eviction
     * can free that memory. Lock the WT_REF so we can look at its address.
     */
    if (!WT_REF_CAS_STATE(session, ref, WT_REF_DISK, WT_REF_LOCKED))
        return (0);

    /*
     * The page is on disk, so there had better be an address; assert that
     * fact, test at run-time to avoid the core dump.
     *
     * Internal pages must be read to walk the tree; ask the block-manager
     * if it's useful to rewrite leaf pages, don't do the I/O if a rewrite
     * won't help.
     */
    __wt_ref_info(ref, &addr, &addr_size, &type);
    WT_ASSERT(session, addr != NULL);
    if (addr != NULL && type != WT_CELL_ADDR_INT) {
        bm = S2BT(session)->bm;
        ret = bm->compact_page_skip(bm, session, addr, addr_size, skipp);
    }

    /* Reset the WT_REF state. */
    WT_REF_SET_STATE(ref, WT_REF_DISK);

    return (ret);
}
