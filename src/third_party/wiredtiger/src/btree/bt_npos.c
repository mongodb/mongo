/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 *
 * Below is a set of functions for computing the normalized position of a page and restoring a page
 * from its normalized position. These functions are used by the eviction server for the sake of
 * not holding the hazard pointer for longer than necessary. Another use is being able to seek to a
 * page at a fraction of the entire dataset.
 *
 * Normalized position is a number in the range of 0 .. 1 that represents a page's position across
 * all pages. It's primary goal is to be cheap rather than precise. It works best when the
 * tree is perfectly balanced, i.e. all internal pages at the same level have the same number of
 * children and the depth of all leaf pages is the same. In practice, the tree is not perfect, so
 * the normalized position is imprecise. However, it's totally fine for the eviction server because
 * it only uses an approximate position in the tree to continue to walk from.
 * Even when using a hazard pointer, page splits can shift data so that some pages or sub-trees can
 * be skipped an eviction pass.
 *
 * Eviction wants to be as non-intrusive as possible and never loads pages into memory, while
 * seek-for-read can load pages or wait for them to be unlocked. The behavior is controlled by
 * the flags passed to the functions. The overall set of flags is quite complex.
 * To simplify the use of this machinery, two helper functions are provided:
 *   - __wt_page_from_npos_for_eviction
 *   - __wt_page_from_npos_for_read
 *
 *    === Detailed description.
 *
 * Normalized position is a number in the range of 0 .. 1 defining a page's position in the tree.
 * In fact, each page occupies a range of positions. For example, if there are 5 pages then
 * positions of pages are:
 *   - [0.0 .. 0.2) -> page 0
 *   - [0.2 .. 0.4) -> page 1
 *   - [0.4 .. 0.6) -> page 2
 *   - [0.6 .. 0.8) -> page 3
 *   - [0.8 .. 1.0] -> page 4
 * The starting point is inclusive, the ending point is exclusive.
 *
 * When calculating a page's position, the returned result is always in the range of 0 .. 1. Because
 * of that, any number outside of this range can be used as an invalid position when storing it.
 *
 * When retrieving a page, any number below 0 will lead to the first page, any number above 1 will
 * lead to the last page. This has useful consequences discussed below.
 *
 *    === Finding a page from its normalized position.
 *
 * If all leaf pages are attached straight to the root, then finding a page from its normalized
 * position is just as simple as multiplying it by the number of leaf pages and using the integer
 * part of the result as the page's index.
 *
 * If there are multiple levels, then the process is similar: the integer part is used as an index
 * at the current level, and the fractional part is used as a normalized position at the next level.
 *
 * This process is repeated until we reach a leaf page.
 *
 * The remaining fractional part at the leaf page can be potentially used to find an exact key
 * on the page. This is not implemented since there's no need for it.
 *
 *    === Calculating a page's normalized position.
 *
 * As opposed to finding a page from its normalized position, the process goes back from the page
 * up to the root. The process is a reverse of the finding process.
 *
 * A nuance is that because there is a whole range of numbers corresponding to a page, the user
 * can choose a starting position within the page. Say, numbers closer to 0 will point to somewhere
 * closer to the beginning of the page, and numbers closer to 1 will point close to the end.
 *
 * If there's only one level, then the normalized position is just the page's index (plus fractional
 * starting point) divided by the number of pages at the parent level.
 *
 * If there are multiple levels, the process is repeated until we reach the root with starting
 * point being whatever has been calculated at the previous level.
 *
 * NOTE that starting points 0 and 1 are corner cases and can lead you to an adjacent page when
 * retrieving a page because of rounding errors.
 * To reliably get back to the same page, the best starting point is 0.5.
 *
 * A useful side effect is that using starting numbers outside of 0 .. 1 range will lead you to
 * adjacent pages. This can be used to iterate over pages without storing any hazard pointers.
 * An example of this can be found in test.
 *
 * Here's an example: Suppose we have a depth 2 tree, at the lowest level we are the 6th out of 10
 * pages and at the higher level the 3rd out of 5 pages. Furthermore we want to start around quarter
 * of the way into the page.
 *
 * Initially we have a starting position of 0.25, our page is at the 6/10th position in this level
 * of the tree. This gives us the following calculation:
 * Level 2 position = (6 + 0.25)/10  = 0.625
 *
 * Then we add the position from level 1:
 * Final position = (3 + Level 2 position)/5 = (3 + 0.625)/5 = 0.725.
 *
 *   === Precision considerations.
 *
 * Because the precision of the position if affected by tree's structure, it can be used to quantify
 * the shape of the tree. The integral difference of all page's normalized positions and their
 * actual positions can be used to estimate the tree's quality.
 *
 * Note that the tree shape in memory can significantly diverge from the tree shape on disk.
 *
 *   === How many pages can be addressed by a double precision number?
 *
 * The maximum number of pages that can be addressed by a double is roughly 2^53 = ~ 10^16
 * (where 53 is the number of bits in a double mantissa).
 * We have multiple orders of magnitude spare by now.
 *
 * For distributed storage it still can be not enough (the dataset size can exceed
 * petabytes or exabytes), then we can shift to using 64-bit or 128-bit fixed-point numbers.
 *
 */

/*
 * !!!
 * __wt_page_npos --
 *     Get the page's normalized position in the tree.
 *     - If 'path_str_offsetp' is set, return a string representation of the page's path.
 *     - 'start' is a position within the leaf page: 0 .. 1.
 *       * When calculating a leaf page's position, use 0.5 to get the middle of the page.
 *       * 0 and 1 are corner cases and can lead you to an adjacent page.
 *       * Numbers outside of 0 .. 1 range will lead you to a prev/next page.
 */
double
__wt_page_npos(WT_SESSION_IMPL *session, WT_REF *ref, double start, char *path_str,
  size_t *path_str_offsetp, size_t path_str_sz_max)
{
    WT_PAGE_INDEX *pindex;
    double npos;
    uint32_t entries, slot;
    int unused = 1; /* WT_UNUSED(snprintf) is cooked in GCC. */

    npos = start;
    if (path_str)
        *path_str_offsetp = 0;

    WT_ENTER_PAGE_INDEX(session);
    while (!__wt_ref_is_root(ref)) {
        slot = UINT32_MAX; /* We get this invalid value in case of error. */
        __wt_ref_index_slot(session, ref, &pindex, &slot);
        entries = pindex->entries;
        /*
         * Depending on the implementation, '__wt_ref_index_slot' might return an error or 'slot'
         * outside of range. Check for 'slot < entries' ensures that it's a valid number. If it's
         * not, then just skip the adjustment: the resulting number will be wrong but still within
         * the range of the current page. Alternatively, could assign it any "reasonable" estimate
         * like 0.5 or 0 / 1 depending on the walk direction.
         */
        if (slot < entries)
            npos = (slot + npos) / entries;
        if (path_str)
            WT_UNUSED(unused = __wt_snprintf_len_incr(&path_str[*path_str_offsetp],
                        path_str_sz_max - *path_str_offsetp, path_str_offsetp,
                        "[%" PRIu32 "/%" PRIu32 "]", slot, entries));
        __wt_ref_ascend(session, &ref, NULL, NULL);
    }
    WT_LEAVE_PAGE_INDEX(session);

    if (path_str)
        path_str[*path_str_offsetp] = 0;

    return (WT_CLAMP(npos, 0.0, 1.0));
}

/*
 * !!!
 * __find_closest_leaf --
 *     Find the closest suitable page according to flags.
 *     - It should not be deleted.
 *     - If WT_READ_CACHE is set, the page should be in memory.
 *     - If the initial ref is to a good page, it will be returned.
 *     - If the initial ref is null, it does nothing.
 */
static int
__find_closest_leaf(WT_SESSION_IMPL *session, WT_REF **refp, uint32_t flags)
{
    WT_DECL_RET;
    uint64_t walkcnt;

    if (*refp == NULL || F_ISSET(*refp, WT_REF_FLAG_LEAF))
        return (0);
    LF_SET(WT_READ_SKIP_INTL);

    ret = __wt_tree_walk_count(session, refp, &walkcnt, flags);

    if (LF_ISSET(WT_READ_EVICT_WALK_FLAGS))
        WT_STAT_CONN_INCR(session, npos_evict_walk_max);
    else
        WT_STAT_CONN_INCR(session, npos_read_walk_max);

    return (ret);
}

/*
 * __page_from_npos_internal --
 *     Go to a leaf page given its normalized position. Note that this function can return a "bad"
 *     page (deleted, locked, etc). The caller of this function should walk the tree to find a
 *     suitable page.
 *
 * NOTE: Must be called within WT_WITH_PAGE_INDEX or WT_ENTER_PAGE_INDEX
 */
static int
__page_from_npos_internal(WT_SESSION_IMPL *session, WT_REF **refp, double npos, uint32_t flags)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_PAGE *page;
    WT_PAGE_INDEX *pindex;
    WT_REF *current, *descent;
    double npos_local;
    int idx, entries;
    bool read_cache;

    *refp = NULL;

    btree = S2BT(session);
    current = NULL;
    /*
     * This function is called by eviction to find a page in the cache. That case is indicated by
     * the WT_READ_CACHE flag. Ordinary lookups in a tree will read pages into cache as needed.
     */
    read_cache = LF_ISSET(WT_READ_CACHE);

restart: /* Restart the search from the root. */

    /* Search the internal pages of the tree. */
    current = &btree->root;
    npos_local = npos;
    for (;;) {
        /*
         * This function will always return a leaf page even if the saved position was for an
         * internal one. This potentially can lead to internal pages being skipped by eviction in
         * case when eviction happened to pause on an internal page and some subsequent internal
         * pages don't have any leafs and also are subject for eviction.
         *
         * This is a rare case. However, it doesn't lead to completely non-evictable internal pages
         * because eviction will eventually reach these pages during other passes.
         */
        if (F_ISSET(current, WT_REF_FLAG_LEAF))
            goto done;

        /* The entire for loop's body is for INTERNAL pages only */

        page = current->page;
        WT_INTL_INDEX_GET(session, page, pindex);
        entries = (int)pindex->entries;

        npos_local *= entries;
        idx = (int)npos_local;
        idx = WT_CLAMP(idx, 0, entries - 1);
        npos_local -= idx;
        descent = pindex->index[idx];

        if (read_cache) {
            /*
             * In case of eviction, we never want to load pages from disk. Also, page_swap with
             * WT_READ_CACHE will fail anyway and we'll lose our pointer, so avoid making a call
             * that will fail.
             */
            switch (WT_REF_GET_STATE(descent)) {
            case WT_REF_DISK:
            case WT_REF_LOCKED:
            case WT_REF_DELETED:
                /* Can't go down from here but it's ok to return the "current" page. */
                goto done;
            default: /* WT_REF_MEM, WT_REF_SPLIT */
                goto descend;
            }
            /* Unreachable but it's ok to be here. */
        } else {
            /* Not eviction */
            switch (WT_REF_GET_STATE(descent)) {
            case WT_REF_LOCKED:
                if (!LF_ISSET(WT_READ_NO_WAIT)) {
                    WT_RET(__wt_page_release(session, current, flags));
                    __wt_sleep(0, 10);
                    goto restart;
                }
                /* Fall through */
            case WT_REF_DELETED:
                /*
                 * Can't go down from here. Return the "current" page and
                 * __find_closest_leaf will finish the job.
                 */
                goto done;
            default: /* WT_REF_DISK, WT_REF_MEM, WT_REF_SPLIT */
                goto descend;
            }
            /* Unreachable but it's ok to be here. */
        }
        /* Unreachable but it's ok to be here. */

descend:
        /*
         * Swap the current page for the child page. If the page splits while we're retrieving it,
         * restart the search at the root.
         *
         * On other error, simply return, the swap call ensures we're holding nothing on failure.
         */
        if ((ret = __wt_page_swap(session, current, descent, flags)) == 0) {
            current = descent;
            continue;
        }
        if (read_cache && (ret == WT_NOTFOUND || ret == WT_RESTART))
            goto done;
        if (ret == WT_RESTART) {
            WT_RET(__wt_page_release(session, current, flags));
            goto restart;
        }
        return (ret);
    }
done:
    /*
     * Because eviction considers internal pages in post-order, returning the root page will
     * indicate the end of walk. Also, eviction will never evict the root. Also, returning a NULL is
     * not an error for eviction but a signal to start over. So handle this case individually.
     */
    if (read_cache && __wt_ref_is_root(current)) {
        WT_RET(__wt_page_release(session, current, flags));
        current = NULL;
    }
    *refp = current;
    return (0);
}

/*
 * __wt_page_from_npos --
 *     Find a page given its normalized position.
 */
int
__wt_page_from_npos(
  WT_SESSION_IMPL *session, WT_REF **refp, double npos, uint32_t read_flags, uint32_t walk_flags)
{
    WT_DECL_RET;

    WT_WITH_PAGE_INDEX(session, ret = __page_from_npos_internal(session, refp, npos, read_flags));
    WT_RET(ret);
    /* Return the first good page starting from here. */
    return (__find_closest_leaf(session, refp, walk_flags));
}

/*
 * !!!
 * __wt_page_from_npos_for_eviction --
 *     Go to a page given its normalized position (for eviction).
 *     - Use WT_READ_PREV to look up backwards.
 */
int
__wt_page_from_npos_for_eviction(
  WT_SESSION_IMPL *session, WT_REF **refp, double npos, uint32_t read_flags, uint32_t walk_flags)
{
    return (__wt_page_from_npos(session, refp, npos, read_flags | WT_READ_EVICT_READ_FLAGS,
      walk_flags | WT_READ_EVICT_WALK_FLAGS));
}

/*
 * !!!
 * __wt_page_from_npos_for_read --
 *     Go to a leaf page given its normalized position (for reading).
 *     - Use WT_READ_PREV to look up backwards.
 */
int
__wt_page_from_npos_for_read(
  WT_SESSION_IMPL *session, WT_REF **refp, double npos, uint32_t read_flags, uint32_t walk_flags)
{
    return (__wt_page_from_npos(
      session, refp, npos, read_flags | WT_READ_DATA_FLAGS, walk_flags | WT_READ_DATA_FLAGS));
}
