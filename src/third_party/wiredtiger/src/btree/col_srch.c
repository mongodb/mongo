/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __check_leaf_key_range --
 *     Check the search key is in the leaf page's key range.
 */
static inline int
__check_leaf_key_range(WT_SESSION_IMPL *session, uint64_t recno, WT_REF *leaf, WT_CURSOR_BTREE *cbt)
{
    WT_PAGE_INDEX *pindex;
    uint32_t indx;

    /*
     * There are reasons we can't do the fast checks, and we continue with the leaf page search in
     * those cases, only skipping the complete leaf page search if we know it's not going to work.
     */
    cbt->compare = 0;

    /*
     * Check if the search key is smaller than the parent's starting key for this page.
     */
    if (recno < leaf->ref_recno) {
        cbt->compare = 1; /* page keys > search key */
        return (0);
    }

    /*
     * Check if the search key is greater than or equal to the starting key
     * for the parent's next page.
     *
     * !!!
     * Check that "indx + 1" is a valid page-index entry first, because it
     * also checks that "indx" is a valid page-index entry, and we have to
     * do that latter check before looking at the indx slot of the array
     * for a match to leaf (in other words, our page hint might be wrong).
     */
    WT_INTL_INDEX_GET(session, leaf->home, pindex);
    indx = leaf->pindex_hint;
    if (indx + 1 < pindex->entries && pindex->index[indx] == leaf)
        if (recno >= pindex->index[indx + 1]->ref_recno) {
            cbt->compare = -1; /* page keys < search key */
            return (0);
        }

    return (0);
}

/*
 * __wt_col_search --
 *     Search a column-store tree for a specific record-based key.
 */
int
__wt_col_search(
  WT_CURSOR_BTREE *cbt, uint64_t search_recno, WT_REF *leaf, bool leaf_safe, bool *leaf_foundp)
{
    WT_BTREE *btree;
    WT_COL *cip;
    WT_DECL_RET;
    WT_INSERT *ins;
    WT_INSERT_HEAD *ins_head;
    WT_PAGE *page;
    WT_PAGE_INDEX *pindex, *parent_pindex;
    WT_REF *current, *descent;
    WT_SESSION_IMPL *session;
    uint64_t recno;
    uint32_t base, indx, limit, read_flags;
    int depth;

    session = CUR2S(cbt);
    btree = S2BT(session);
    current = NULL;

    /*
     * Assert the session and cursor have the right relationship (not search specific, but search is
     * a convenient place to check given any operation on a cursor will likely search a page).
     */
    WT_ASSERT(session, session->dhandle == cbt->dhandle);

    __cursor_pos_clear(cbt);

    /*
     * When appending a new record, the search record number will be an out-of-band value, search
     * for the largest key in the table instead.
     */
    if ((recno = search_recno) == WT_RECNO_OOB)
        recno = UINT64_MAX;

    /*
     * We may be searching only a single leaf page, not the full tree. In the normal case where we
     * are searching a tree, check the page's parent keys before doing the full search, it's faster
     * when the cursor is being re-positioned. Skip that check if we know the page is the right one
     * (for example, when re-instantiating a page in memory, in that case we know the target must be
     * on the current page).
     */
    if (leaf != NULL) {
        WT_ASSERT(session, search_recno != WT_RECNO_OOB);

        if (!leaf_safe) {
            WT_RET(__check_leaf_key_range(session, recno, leaf, cbt));
            *leaf_foundp = cbt->compare == 0;
            if (!*leaf_foundp)
                return (0);
        }

        current = leaf;
        goto leaf_only;
    }

    if (0) {
restart:
        /*
         * Discard the currently held page and restart the search from the root.
         */
        WT_RET(__wt_page_release(session, current, 0));
    }

    /* Search the internal pages of the tree. */
    current = &btree->root;
    for (depth = 2, pindex = NULL;; ++depth) {
        parent_pindex = pindex;
        page = current->page;
        if (page->type != WT_PAGE_COL_INT)
            break;

        WT_INTL_INDEX_GET(session, page, pindex);
        base = pindex->entries;
        descent = pindex->index[base - 1];

        /* Fast path appends. */
        if (recno >= descent->ref_recno) {
            /*
             * If on the last slot (the key is larger than any key on the page), check for an
             * internal page split race.
             */
            if (__wt_split_descent_race(session, current, parent_pindex))
                goto restart;

            goto descend;
        }

        /* Binary search of internal pages. */
        for (base = 0, limit = pindex->entries - 1; limit != 0; limit >>= 1) {
            indx = base + (limit >> 1);
            descent = pindex->index[indx];

            if (recno == descent->ref_recno)
                break;
            if (recno < descent->ref_recno)
                continue;
            base = indx + 1;
            --limit;
        }
descend:
        /*
         * Reference the slot used for next step down the tree.
         *
         * Base is the smallest index greater than recno and may be the (last + 1) index. The slot
         * for descent is the one before base.
         */
        if (recno != descent->ref_recno) {
            /*
             * We don't have to correct for base == 0 because the only way for base to be 0 is if
             * recno is the page's starting recno.
             */
            WT_ASSERT(session, base > 0);
            descent = pindex->index[base - 1];
        }

        /* Encourage races. */
        WT_DIAGNOSTIC_YIELD;

        /*
         * Swap the current page for the child page. If the page splits while we're retrieving it,
         * restart the search at the root. We cannot restart in the "current" page; for example, if
         * a thread is appending to the tree, the page it's waiting for did an insert-split into the
         * parent, then the parent split into its parent, the name space we are searching for may
         * have moved above the current page in the tree.
         *
         * On other error, simply return, the swap call ensures we're holding nothing on failure.
         */
        read_flags = WT_READ_RESTART_OK;
        if (F_ISSET(cbt, WT_CBT_READ_ONCE))
            FLD_SET(read_flags, WT_READ_WONT_NEED);
        if ((ret = __wt_page_swap(session, current, descent, read_flags)) == 0) {
            current = descent;
            continue;
        }
        if (ret == WT_RESTART)
            goto restart;
        return (ret);
    }

    /* Track how deep the tree gets. */
    if (depth > btree->maximum_depth)
        btree->maximum_depth = depth;

leaf_only:
    page = current->page;
    cbt->ref = current;

    /*
     * Don't bother searching if the caller is appending a new record where we'll allocate the
     * record number; we're not going to find a match by definition, and we figure out the record
     * number and position when we do the work.
     */
    if (search_recno == WT_RECNO_OOB) {
        cbt->compare = -1;
        return (0);
    }

    /*
     * Search the leaf page.
     *
     * Search after a page is pinned does a search of the pinned page before doing a full tree
     * search, in which case we might be searching for a record logically before the page. Return
     * failure, and there's nothing else to do, the record isn't going to be on this page.
     *
     * We don't check inside the search path for a record greater than the maximum record in the
     * tree; in that case, we get here with a record that's impossibly large for the page. We do
     * have additional setup to do in that case, the record may be appended to the page.
     */
    if (page->type == WT_PAGE_COL_FIX) {
        if (recno < current->ref_recno) {
            cbt->recno = current->ref_recno;
            cbt->slot = 0;
            cbt->compare = 1;
            return (0);
        }
        if (recno >= current->ref_recno + page->entries) {
            cbt->recno = current->ref_recno + page->entries;
            cbt->slot = 0;
            goto past_end;
        } else {
            cbt->recno = recno;
            cbt->slot = 0;
            cbt->compare = 0;
            ins_head = WT_COL_UPDATE_SINGLE(page);
        }
    } else {
        if (recno < current->ref_recno) {
            cbt->recno = current->ref_recno;
            cbt->slot = 0;
            cbt->compare = 1;
            return (0);
        }
        if ((cip = __col_var_search(current, recno, NULL)) == NULL) {
            cbt->recno = __col_var_last_recno(current);
            cbt->slot = page->entries == 0 ? 0 : page->entries - 1;
            goto past_end;
        } else {
            cbt->recno = recno;
            cbt->slot = WT_COL_SLOT(page, cip);
            cbt->compare = 0;
            ins_head = WT_COL_UPDATE_SLOT(page, cbt->slot);
            F_SET(cbt, WT_CBT_VAR_ONPAGE_MATCH);
        }
    }

    /*
     * We have a match on the page, check for an update. Check the page's update list
     * (fixed-length), or slot's update list (variable-length) for a better match. The only better
     * match we can find is an exact match, otherwise the existing match on the page is the one we
     * want. For that reason, don't set the cursor's WT_INSERT_HEAD/WT_INSERT pair until we know we
     * have a useful entry.
     */
    if ((ins = __col_insert_search(ins_head, cbt->ins_stack, cbt->next_stack, recno)) != NULL)
        if (recno == WT_INSERT_RECNO(ins)) {
            cbt->ins_head = ins_head;
            cbt->ins = ins;
        }
    return (0);

past_end:
    /* We don't always set these below, add a catch-all. */
    cbt->ins_head = NULL;
    cbt->ins = NULL;

    /*
     * A record past the end of the page's standard information. Check the append list; by
     * definition, any record on the append list is closer than the last record on the page, so it's
     * a better choice for return. This is a rarely used path: we normally find exact matches,
     * because column-store files are dense, but in this case the caller searched past the end of
     * the table.
     */
    ins_head = WT_COL_APPEND(page);
    ins = __col_insert_search(ins_head, cbt->ins_stack, cbt->next_stack, recno);
    if (ins == NULL) {
        /*
         * There is nothing on the append list, so search the insert list. (The append list would
         * have been closer to the search record).
         */
        if (cbt->recno != WT_RECNO_OOB) {
            if (page->type == WT_PAGE_COL_FIX)
                ins_head = WT_COL_UPDATE_SINGLE(page);
            else {
                ins_head = WT_COL_UPDATE_SLOT(page, cbt->slot);

                /*
                 * Set this, otherwise the code in cursor_valid will assume there's no on-disk value
                 * underneath ins_head.
                 */
                F_SET(cbt, WT_CBT_VAR_ONPAGE_MATCH);
            }

            ins = WT_SKIP_LAST(ins_head);
            if (ins != NULL && cbt->recno == WT_INSERT_RECNO(ins)) {
                cbt->ins_head = ins_head;
                cbt->ins = ins;
            }
        }

        cbt->compare = -1;
    } else {
        WT_ASSERT(session, page->type == WT_PAGE_COL_FIX || !F_ISSET(cbt, WT_CBT_VAR_ONPAGE_MATCH));

        cbt->ins_head = ins_head;
        cbt->ins = ins;
        cbt->recno = WT_INSERT_RECNO(cbt->ins);
        if (recno == cbt->recno)
            cbt->compare = 0;
        else if (recno < cbt->recno)
            cbt->compare = 1;
        else
            cbt->compare = -1;
    }
    return (0);
}
