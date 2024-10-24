/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __split_prev_race --
 *     Check for races when descending the tree during a previous-cursor walk.
 */
static WT_INLINE bool
__split_prev_race(WT_SESSION_IMPL *session, WT_REF *ref, WT_PAGE_INDEX **pindexp)
{
    WT_PAGE_INDEX *pindex;

    /*
     * Handle a cursor moving backwards through the tree or setting up at the end of the tree. We're
     * passed the child page into which we're descending, and the parent page's page-index we used
     * to find that child page.
     *
     * When splitting an internal page into its parent, we move the split pages WT_REF structures,
     * then update the parent's page index, then update the split page's page index, and nothing is
     * atomic. A thread can read the parent page's replacement page index and then the split page's
     * original index, or vice-versa, and either change can cause a cursor moving backwards through
     * the tree to skip pages.
     *
     * This isn't a problem for a cursor setting up at the start of the tree or moving forward
     * through the tree because we do right-hand splits on internal pages and the initial part of
     * the split page's namespace won't change as part of a split (in other words, a thread reading
     * the parent page's and split page's indexes will move to the same slot no matter what order of
     * indexes are read.
     *
     * Acquire the child's page index, then confirm the parent's page index hasn't changed, to check
     * for reading an old version of the parent's page index and then reading a new version of the
     * child's page index.
     */
    WT_INTL_INDEX_GET(session, ref->page, pindex);
    if (__wt_split_descent_race(session, ref, *pindexp))
        return (true);

    /*
     * That doesn't check if we read a new version of parent's page index
     * and then an old version of the child's page index. For example, if
     * a thread were in a newly created split page subtree, the split
     * completes into the parent before the thread reads it and descends
     * into the child (where the split hasn't yet completed).
     *
     * Imagine an internal page with 3 child pages, with the namespaces a-f,
     * g-h and i-j; the first child page splits. The parent starts out with
     * the following page-index:
     *
     *	| ... | a | g | i | ... |
     *
     * The split page starts out with the following page-index:
     *
     *	| a | b | c | d | e | f |
     *
     * The first step is to move the c-f ranges into a new subtree, so, for
     * example we might have two new internal pages 'c' and 'e', where the
     * new 'c' page references the c-d namespace and the new 'e' page
     * references the e-f namespace. The top of the subtree references the
     * parent page, but until the parent's page index is updated, threads in
     * the subtree won't be able to ascend out of the subtree. However, once
     * the parent page's page index is updated to this:
     *
     *	| ... | a | c | e | g | i | ... |
     *
     * threads in the subtree can ascend into the parent. Imagine a cursor
     * in the c-d part of the namespace that ascends to the parent's 'c'
     * slot. It would then decrement to the slot before the 'c' slot, the
     * 'a' slot.
     *
     * The previous-cursor movement selects the last slot in the 'a' page;
     * if the split page's page-index hasn't been updated yet, it selects
     * the 'f' slot, which is incorrect. Once the split page's page index is
     * updated to this:
     *
     *	| a | b |
     *
     * the previous-cursor movement will select the 'b' slot, which is
     * correct.
     *
     * If the last slot on the page no longer points to the current page as
     * its "home", the page is being split and part of its namespace moved,
     * restart. (We probably don't have to restart, I think we could spin
     * until the page-index is updated, but I'm not willing to debug that
     * one if I'm wrong.)
     */
    if (pindex->index[pindex->entries - 1]->home != ref->page)
        return (true);

    *pindexp = pindex;
    return (false);
}

/*
 * __tree_walk_internal --
 *     Move to the next/previous page in the tree, skipping pages in the WT_REF_DELETED state and
 *     for other reasons. Those other reasons are generally controlled by the flags passed in to
 *     this function.
 */
static WT_INLINE int
__tree_walk_internal(WT_SESSION_IMPL *session, WT_REF **refp, uint64_t *walkcntp,
  int (*skip_func)(WT_SESSION_IMPL *, WT_REF *, void *, bool, bool *), void *func_cookie,
  uint32_t flags)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_PAGE_INDEX *pindex;
    WT_REF *couple, *ref, *ref_orig;
    WT_REF_STATE current_state;
    uint64_t restart_sleep, restart_yield;
    uint32_t slot;
    bool empty_internal, prev, skip;

    btree = S2BT(session);
    pindex = NULL;
    restart_sleep = restart_yield = 0;
    empty_internal = false;

    /* Ensure we have a snapshot to check visibility or we only check global visibility. */
    WT_ASSERT(session, LF_ISSET(WT_READ_VISIBLE_ALL) || F_ISSET(session->txn, WT_TXN_HAS_SNAPSHOT));

    /*
     * Historically, all tree walks skipped deleted pages. There are now some exceptions to this:
     * Rollback to stable, and column store append. Rather than add the read-see-deleted flag to
     * every single tree walk call, we hide these pages unless:
     *
     * 1. We detect that rollback to stable is in progress
     * 2. Callers opt into seeing these pages with the read-see-deleted flag.
     *
     * Ideally, rollback to stable would also use the read-see-deleted flag but it uses cursor->next
     * and cursor->prev, which don't have flags.
     */
    if (!F_ISSET(session, WT_SESSION_ROLLBACK_TO_STABLE) && !LF_ISSET(WT_READ_SEE_DELETED))
        LF_SET(WT_READ_SKIP_DELETED);

    /*
     * !!!
     * Fast-truncate does not currently work for FLCS trees.
     */
    if (btree->type == BTREE_COL_FIX)
        LF_CLR(WT_READ_TRUNCATE);

    prev = LF_ISSET(WT_READ_PREV) ? 1 : 0;

    /*
     * There are multiple reasons and approaches to walking the in-memory
     * tree:
     *
     * (1) finding pages to evict (the eviction server);
     * (2) writing just dirty leaves or internal nodes (checkpoint);
     * (3) discarding pages (close);
     * (4) truncating pages in a range (fast truncate);
     * (5) skipping pages based on outside information (compaction);
     * (6) cursor scans (applications).
     *
     * Except for cursor scans and compaction, the walk is limited to the
     * cache, no pages are read.  In all cases, hazard pointers protect the
     * walked pages from eviction.
     *
     * Walks use hazard-pointer coupling through the tree and that's OK
     * (hazard pointers can't deadlock, so there's none of the usual
     * problems found when logically locking up a btree).  If the eviction
     * thread tries to evict the active page, it fails because of our
     * hazard pointer.  If eviction tries to evict our parent, that fails
     * because the parent has a child page that can't be discarded.  We do
     * play one game: don't couple up to our parent and then back down to a
     * new leaf, couple to the next page to which we're descending, it
     * saves a hazard-pointer swap for each cursor page movement.
     *
     * The hazard pointer on the original location is held until the end of
     * the movement, in case we have to restart the movement. Take a copy
     * of any held page and clear the return value (it makes future error
     * handling easier).
     */
    couple = NULL;
    ref_orig = *refp;
    *refp = NULL;

    /*
     * Tree walks are special: they look inside page structures that splits may want to free.
     */
    WT_ENTER_PAGE_INDEX(session);

    /* If no page is active, begin a walk from the start/end of the tree. */
    if ((ref = ref_orig) == NULL) {
        if (0) {
restart:
            /*
             * Yield before retrying, and if we've yielded enough times, start sleeping so we don't
             * burn CPU to no purpose.
             */
            __wt_spin_backoff(&restart_yield, &restart_sleep);

            WT_ERR(__wt_page_release(session, couple, flags));
            couple = NULL;
        }

        if ((ref = ref_orig) == NULL) {
            ref = &btree->root;
            WT_INTL_INDEX_GET(session, ref->page, pindex);
            slot = prev ? pindex->entries - 1 : 0;
            goto descend;
        }
    }

    /*
     * If the active page was the root, we've reached the walk's end; we only get here if we've
     * returned the root to our caller, so we're holding no hazard pointers.
     */
    if (__wt_ref_is_root(ref))
        goto done;

    /* Figure out the current slot in the WT_REF array. */
    __wt_ref_index_slot(session, ref, &pindex, &slot);

    for (;;) {
        /*
         * If we're at the last/first slot on the internal page, return it in post-order traversal.
         * Otherwise move to the next/prev slot and left/right-most element in that subtree.
         */
        while ((prev && slot == 0) || (!prev && slot == pindex->entries - 1)) {
            /* Ascend to the parent. */
            __wt_ref_ascend(session, &ref, &pindex, &slot);

            /*
             * If at the root and returning internal pages, return the root page, otherwise we're
             * done.
             */
            if (__wt_ref_is_root(ref)) {
                if (!LF_ISSET(WT_READ_SKIP_INTL)) {
                    *refp = ref;
                    WT_ASSERT(session, ref != ref_orig);
                }
                goto done;
            }

            /*
             * If we got all the way through an internal page and all of the child pages were
             * deleted, mark it for eviction.
             */
            if (empty_internal) {
                __wt_evict_page_soon(session, ref);
                empty_internal = false;
            }

            /* Optionally return internal pages. */
            if (LF_ISSET(WT_READ_SKIP_INTL))
                continue;

            /*
             * Swap our previous hazard pointer for the page we'll return.
             *
             * Not-found is an expected return, as eviction might have been attempted. Restart is
             * not expected, our parent WT_REF should not have split.
             */
            WT_ERR_NOTFOUND_OK(
              __wt_page_swap(session, couple, ref, WT_READ_NOTFOUND_OK | flags), true);
            if (ret == 0) {
                /* Success, "couple" released. */
                couple = NULL;
                *refp = ref;
                WT_ASSERT(session, ref != ref_orig);

                if (__wt_session_prefetch_check(session, ref))
                    WT_ERR(__wti_btree_prefetch(session, ref));

                goto done;
            }

            /* ret == WT_NOTFOUND, an expected error.  Continue with "couple" unchanged. */
        }

        if (prev)
            --slot;
        else
            ++slot;

        if (walkcntp != NULL)
            ++*walkcntp;

        for (;;) {
descend:
            /*
             * Get a reference, setting the reference hint if it's wrong (used when we continue the
             * walk). We don't always update the hints when splitting, it's expected for them to be
             * incorrect in some workloads.
             */
            ref = pindex->index[slot];
            if (ref->pindex_hint != slot)
                ref->pindex_hint = slot;

            /*
             * If we see any child states other than deleted, the page isn't empty.
             */
            current_state = WT_REF_GET_STATE(ref);
            if (current_state != WT_REF_DELETED && !LF_ISSET(WT_READ_TRUNCATE))
                empty_internal = false;

            if (LF_ISSET(WT_READ_CACHE)) {
                /*
                 * Only look at unlocked pages in memory.
                 */
                if (LF_ISSET(WT_READ_NO_WAIT) && current_state != WT_REF_MEM)
                    break;
            } else if (LF_ISSET(WT_READ_TRUNCATE)) {
                /*
                 * If deleting a range, try to delete the page without instantiating it. (Note this
                 * test follows the check to skip the page entirely if it's already deleted.)
                 */
                WT_ERR(__wti_delete_page(session, ref, &skip));
                if (skip)
                    break;
                empty_internal = false;
            } else if (LF_ISSET(WT_READ_SKIP_DELETED) && current_state == WT_REF_DELETED) {
                /*
                 * Try to skip deleted pages visible to us.
                 */
                if (__wti_delete_page_skip(session, ref, LF_ISSET(WT_READ_VISIBLE_ALL)))
                    break;
            }

            /* See if our caller wants to skip this page. */
            if (skip_func != NULL) {
                WT_ERR(skip_func(session, ref, func_cookie, LF_ISSET(WT_READ_VISIBLE_ALL), &skip));
                if (skip)
                    break;
            }

            ret = __wt_page_swap(
              session, couple, ref, WT_READ_NOTFOUND_OK | WT_READ_RESTART_OK | flags);
            if (ret == 0) {
                /* Success, so "couple" has been released. */
                couple = NULL;

                if (__wt_session_prefetch_check(session, ref))
                    WT_ERR(__wti_btree_prefetch(session, ref));

                /* Return leaf pages to our caller. */
                if (F_ISSET(ref, WT_REF_FLAG_LEAF)) {
                    *refp = ref;
                    WT_ASSERT(session, ref != ref_orig);
                    goto done;
                }

                /* Set the new "couple" value. */
                couple = ref;

                /* Configure traversal of any internal page. */
                empty_internal = true;
                if (prev) {
                    if (__split_prev_race(session, ref, &pindex))
                        goto restart;
                    slot = pindex->entries - 1;
                } else {
                    WT_INTL_INDEX_GET(session, ref->page, pindex);
                    slot = 0;
                }
                continue;
            }

            /*
             * Not-found is an expected return when walking only in-cache pages, or if we see a
             * deleted page.
             *
             * An expected error, so "couple" is unchanged.
             */
            if (ret == WT_NOTFOUND) {
                WT_STAT_CONN_INCR(session, eviction_walk_leaf_notfound);
                WT_NOT_READ(ret, 0);
                break;
            }

            /*
             * The page we're moving to might have split, in which case restart the movement.
             *
             * An expected error, so "couple" is unchanged.
             */
            if (ret == WT_RESTART)
                goto restart;

            /* Unexpected error, so "couple" was released. */
            couple = NULL;
            goto err;
        }
    }

done:
err:
    WT_TRET(__wt_page_release(session, couple, flags));
    WT_TRET(__wt_page_release(session, ref_orig, flags));
    WT_LEAVE_PAGE_INDEX(session);
    return (ret);
}

/*
 * __wt_tree_walk --
 *     Move to the next/previous page in the tree.
 */
int
__wt_tree_walk(WT_SESSION_IMPL *session, WT_REF **refp, uint32_t flags)
{
    return (__tree_walk_internal(session, refp, NULL, NULL, NULL, flags));
}

/*
 * __wt_tree_walk_count --
 *     Move to the next/previous page in the tree, tracking how many references were visited to get
 *     there.
 */
int
__wt_tree_walk_count(WT_SESSION_IMPL *session, WT_REF **refp, uint64_t *walkcntp, uint32_t flags)
{
    return (__tree_walk_internal(session, refp, walkcntp, NULL, NULL, flags));
}

/*
 * __wt_tree_walk_custom_skip --
 *     Walk the tree calling a custom function to decide whether to skip refs.
 */
int
__wt_tree_walk_custom_skip(WT_SESSION_IMPL *session, WT_REF **refp,
  int (*skip_func)(WT_SESSION_IMPL *, WT_REF *, void *, bool, bool *), void *func_cookie,
  uint32_t flags)
{
    return (__tree_walk_internal(session, refp, NULL, skip_func, func_cookie, flags));
}

/*
 * __tree_walk_skip_count_callback --
 *     Optionally skip leaf pages. When the skip-leaf-count variable is non-zero, skip some count of
 *     leaf pages, then take the next leaf page we can. The reason to do some of this work here, is
 *     because we can look at the cell and know it's a leaf page without reading it into memory. If
 *     this page is disk-based, crack the cell to figure out it's a leaf page without reading it.
 */
static int
__tree_walk_skip_count_callback(
  WT_SESSION_IMPL *session, WT_REF *ref, void *context, bool visible_all, bool *skipp)
{
    uint64_t *skipleafcntp;

    skipleafcntp = (uint64_t *)context;
    WT_ASSERT(session, skipleafcntp != NULL);

    /*
     * Skip deleted pages visible to us.
     */
    if (WT_REF_GET_STATE(ref) == WT_REF_DELETED &&
      __wti_delete_page_skip(session, ref, visible_all))
        *skipp = true;
    else if (*skipleafcntp > 0 && F_ISSET(ref, WT_REF_FLAG_LEAF)) {
        --*skipleafcntp;
        *skipp = true;
    } else
        *skipp = false;
    return (0);
}

/*
 * __wti_tree_walk_skip --
 *     Move to the next/previous page in the tree, skipping a certain number of leaf pages before
 *     returning.
 */
int
__wti_tree_walk_skip(WT_SESSION_IMPL *session, WT_REF **refp, uint64_t *skipleafcntp)
{
    /*
     * Optionally skip leaf pages, the second half. The tree-walk function didn't have an on-page
     * cell it could use to figure out if the page was a leaf page or not, it had to acquire the
     * hazard pointer and look at the page. The tree-walk code never acquires a hazard pointer on a
     * leaf page without returning it, and it's not trivial to change that. So, the tree-walk code
     * returns all leaf pages here and we deal with decrementing the count.
     */
    do {
        WT_RET(__tree_walk_internal(session, refp, NULL, __tree_walk_skip_count_callback,
          skipleafcntp, WT_READ_INTERNAL_OP | WT_READ_SKIP_INTL | WT_READ_WONT_NEED));

        /*
         * The walk skipped internal pages, any page returned must be a leaf page.
         */
        if (*skipleafcntp > 0)
            --*skipleafcntp;
    } while (*skipleafcntp > 0);

    return (0);
}
