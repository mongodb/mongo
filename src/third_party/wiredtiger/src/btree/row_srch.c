/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static inline int __validate_next_stack(
  WT_SESSION_IMPL *session, WT_INSERT *next_stack[WT_SKIP_MAXDEPTH], WT_ITEM *srch_key);

/*
 * __search_insert_append --
 *     Fast append search of a row-store insert list, creating a skiplist stack as we go.
 */
static inline int
__search_insert_append(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_INSERT_HEAD *ins_head,
  WT_ITEM *srch_key, bool *donep)
{
    WT_BTREE *btree;
    WT_COLLATOR *collator;
    WT_INSERT *ins;
    WT_ITEM key;
    int cmp, i;

    *donep = 0;

    btree = S2BT(session);
    collator = btree->collator;

    if ((ins = WT_SKIP_LAST(ins_head)) == NULL)
        return (0);
    /*
     * Since the head of the skip list doesn't get mutated within this function, the compiler may
     * move this assignment above within the loop below if it needs to (and may read a different
     * value on each loop due to other threads mutating the skip list).
     *
     * Place a read barrier here to avoid this issue.
     */
    WT_READ_BARRIER();
    key.data = WT_INSERT_KEY(ins);
    key.size = WT_INSERT_KEY_SIZE(ins);

    WT_RET(__wt_compare(session, collator, srch_key, &key, &cmp));
    if (cmp >= 0) {
        /*
         * !!!
         * We may race with another appending thread.
         *
         * To catch that case, rely on the atomic pointer read above
         * and set the next stack to NULL here.  If we have raced with
         * another thread, one of the next pointers will not be NULL by
         * the time they are checked against the next stack inside the
         * serialized insert function.
         */
        for (i = WT_SKIP_MAXDEPTH - 1; i >= 0; i--) {
            cbt->ins_stack[i] = (i == 0) ?
              &ins->next[0] :
              (ins_head->tail[i] != NULL) ? &ins_head->tail[i]->next[i] : &ins_head->head[i];
            cbt->next_stack[i] = NULL;
        }
        cbt->compare = -cmp;
        cbt->ins = ins;
        cbt->ins_head = ins_head;

        /*
         * If we find an exact match, copy the key into the temporary buffer, our callers expect to
         * find it there.
         */
        if (cbt->compare == 0) {
            cbt->tmp->data = WT_INSERT_KEY(cbt->ins);
            cbt->tmp->size = WT_INSERT_KEY_SIZE(cbt->ins);
        }

        *donep = 1;
    }
    return (0);
}

/*
 * __wt_search_insert --
 *     Search a row-store insert list, creating a skiplist stack as we go.
 */
int
__wt_search_insert(
  WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_INSERT_HEAD *ins_head, WT_ITEM *srch_key)
{
    WT_BTREE *btree;
    WT_COLLATOR *collator;
    WT_INSERT *ins, **insp, *last_ins;
    WT_ITEM key;
    size_t match, skiphigh, skiplow;
    int cmp, i;

    btree = S2BT(session);
    collator = btree->collator;
    cmp = 0; /* -Wuninitialized */

    /*
     * The insert list is a skip list: start at the highest skip level, then go as far as possible
     * at each level before stepping down to the next.
     */
    match = skiphigh = skiplow = 0;
    ins = last_ins = NULL;
    for (i = WT_SKIP_MAXDEPTH - 1, insp = &ins_head->head[i]; i >= 0;) {
        /*
         * The algorithm requires that the skip list insert pointer is only read once within the
         * loop. While the compiler can change the code in a way that it reads the insert pointer
         * value from memory again in the following code.
         *
         * In addition, a CPU with weak memory ordering, such as ARM, may reorder the reads and read
         * a stale value. It is not OK and the reason is explained in the following comment.
         *
         * Place a read barrier here to avoid these issues.
         */
        WT_ORDERED_READ_WEAK_MEMORDER(ins, *insp);
        if (ins == NULL) {
            cbt->next_stack[i] = NULL;
            cbt->ins_stack[i--] = insp--;
            continue;
        }

        /*
         * Comparisons may be repeated as we drop down skiplist levels; don't repeat comparisons,
         * they might be expensive.
         */
        if (ins != last_ins) {
            last_ins = ins;
            key.data = WT_INSERT_KEY(ins);
            key.size = WT_INSERT_KEY_SIZE(ins);
            /*
             * We have an optimization to reduce the number of bytes we need to compare during the
             * search if we know a prefix of the search key matches the keys we have already
             * compared on the upper stacks. This works because we know the keys become denser down
             * the stack.
             *
             * However, things become tricky if we have another key inserted concurrently next to
             * the search key. The current search may or may not see the concurrently inserted key
             * but it should always see a valid skip list. In other words,
             *
             * 1) at any level of the list, keys are in sorted order;
             *
             * 2) if a reader sees a key in level N, that key is also in all levels below N.
             *
             * Otherwise, we may wrongly skip the comparison of a prefix and land on the wrong spot.
             * Here's an example:
             *
             * Suppose we have a skip list:
             *
             * L1: AA -> BA
             *
             * L0: AA -> BA
             *
             * and we want to search AB and a key AC is inserted concurrently. If we see the
             * following skip list in the search:
             *
             * L1: AA -> AC -> BA
             *
             * L0: AA -> BA
             *
             * Since we have compared with AA and AC on level 1 before dropping down to level 0, we
             * decide we can skip comparing the first byte of the key. However, since we don't see
             * AC on level 0, we compare with BA and wrongly skip the comparison with prefix B.
             *
             * On architectures with strong memory ordering, the requirement is satisfied by
             * inserting the new key to the skip list from lower stack to upper stack using an
             * atomic compare and swap operation, which functions as a full barrier. However, it is
             * not enough on the architecture that has weaker memory ordering, such as ARM.
             * Therefore, an extra read barrier is needed for these platforms.
             */
            match = WT_MIN(skiplow, skiphigh);
            WT_RET(__wt_compare_skip(session, collator, srch_key, &key, &cmp, &match));
        }

        if (cmp > 0) { /* Keep going at this level */
            insp = &ins->next[i];
            skiplow = match;
        } else if (cmp < 0) { /* Drop down a level */
            cbt->next_stack[i] = ins;
            cbt->ins_stack[i--] = insp--;
            skiphigh = match;
        } else
            for (; i >= 0; i--) {
                /*
                 * It is possible that we read an old value down the stack due to read reordering on
                 * CPUs with weak memory ordering. Add a read barrier to avoid this issue.
                 */
                WT_ORDERED_READ_WEAK_MEMORDER(cbt->next_stack[i], ins->next[i]);
                cbt->ins_stack[i] = &ins->next[i];
            }
    }

    /*
     * For every insert element we review, we're getting closer to a better choice; update the
     * compare field to its new value. If we went past the last item in the list, return the last
     * one: that is used to decide whether we are positioned in a skiplist.
     */
    cbt->compare = -cmp;
    cbt->ins = (ins != NULL) ? ins : last_ins;
    cbt->ins_head = ins_head;

    /*
     * This is an expensive call on a performance-critical path, so we only want to enable it behind
     * the stress_skiplist session flag.
     */
    if (FLD_ISSET(S2C(session)->debug_flags, WT_CONN_DEBUG_STRESS_SKIPLIST))
        WT_RET(__validate_next_stack(session, cbt->next_stack, srch_key));

    return (0);
}

/*
 * __validate_next_stack --
 *     Verify that for each level in the provided next_stack that higher levels on the stack point
 *     to larger inserts than lower levels, and all inserts are larger than the srch_key used in
 *     building the next_stack.
 */
static inline int
__validate_next_stack(
  WT_SESSION_IMPL *session, WT_INSERT *next_stack[WT_SKIP_MAXDEPTH], WT_ITEM *srch_key)
{

    WT_ITEM upper_key, lower_key;
    int32_t i, cmp;
    WT_COLLATOR *collator;

    /*
     * Hide the flag check for non-diagnostics builds, too.
     */
#ifndef HAVE_DIAGNOSTIC
    return (0);
#endif

    collator = S2BT(session)->collator;
    WT_CLEAR(upper_key);
    WT_CLEAR(lower_key);
    cmp = 0;

    for (i = WT_SKIP_MAXDEPTH - 2; i >= 0; i--) {

        /* If lower levels point to the end of the skiplist, higher levels must as well. */
        if (next_stack[i] == NULL)
            WT_ASSERT_ALWAYS(session, next_stack[i + 1] == NULL,
              "Invalid next_stack: Level %d is NULL but higher level %d has pointer %p", i, i + 1,
              (void *)next_stack[i + 1]);

        /* We only need to compare when both levels point to different, non-NULL inserts. */
        if (next_stack[i] == NULL || next_stack[i + 1] == NULL ||
          next_stack[i] == next_stack[i + 1])
            continue;

        lower_key.data = WT_INSERT_KEY(next_stack[i]);
        lower_key.size = WT_INSERT_KEY_SIZE(next_stack[i]);

        upper_key.data = WT_INSERT_KEY(next_stack[i + 1]);
        upper_key.size = WT_INSERT_KEY_SIZE(next_stack[i + 1]);

        WT_RET(__wt_compare(session, collator, &upper_key, &lower_key, &cmp));
        WT_ASSERT_ALWAYS(session, cmp >= 0,
          "Invalid next_stack: Lower level points to larger key: Level %d = %s, Level %d = %s", i,
          (char *)lower_key.data, i + 1, (char *)upper_key.data);
    }

    if (next_stack[0] != NULL) {
        /*
         * Finally, confirm that next_stack[0] is greater than srch_key. We've already confirmed
         * that all keys on higher levels are larger than next_stack[0] and therefore also larger
         * than srch_key.
         */
        lower_key.data = WT_INSERT_KEY(next_stack[0]);
        lower_key.size = WT_INSERT_KEY_SIZE(next_stack[0]);

        WT_RET(__wt_compare(session, collator, srch_key, &lower_key, &cmp));
        WT_ASSERT_ALWAYS(session, cmp < 0,
          "Invalid next_stack: Search key is larger than keys on next_stack: srch_key = %s, "
          "next_stack[0] = %s",
          (char *)srch_key->data, (char *)lower_key.data);
    }

    return (0);
}

/*
 * __check_leaf_key_range --
 *     Check the search key is in the leaf page's key range.
 */
static inline int
__check_leaf_key_range(
  WT_SESSION_IMPL *session, WT_ITEM *srch_key, WT_REF *leaf, WT_CURSOR_BTREE *cbt)
{
    WT_BTREE *btree;
    WT_COLLATOR *collator;
    WT_ITEM *item;
    WT_PAGE_INDEX *pindex;
    uint32_t indx;
    int cmp;

    btree = S2BT(session);
    collator = btree->collator;
    item = cbt->tmp;

    /*
     * There are reasons we can't do the fast checks, and we continue with the leaf page search in
     * those cases, only skipping the complete leaf page search if we know it's not going to work.
     */
    cbt->compare = 0;

    /*
     * First, confirm we have the right parent page-index slot, and quit if we don't. We don't
     * search for the correct slot, that would make this cheap test expensive.
     */
    WT_INTL_INDEX_GET(session, leaf->home, pindex);
    indx = leaf->pindex_hint;
    if (indx >= pindex->entries || pindex->index[indx] != leaf)
        return (0);

    /*
     * Check if the search key is smaller than the parent's starting key for this page.
     *
     * We can't compare against slot 0 on a row-store internal page because reconciliation doesn't
     * build it, it may not be a valid key.
     */
    if (indx != 0) {
        __wt_ref_key(leaf->home, leaf, &item->data, &item->size);
        WT_RET(__wt_compare(session, collator, srch_key, item, &cmp));
        if (cmp < 0) {
            cbt->compare = 1; /* page keys > search key */
            return (0);
        }
    }

    /*
     * Check if the search key is greater than or equal to the starting key for the parent's next
     * page.
     */
    ++indx;
    if (indx < pindex->entries) {
        __wt_ref_key(leaf->home, pindex->index[indx], &item->data, &item->size);
        WT_RET(__wt_compare(session, collator, srch_key, item, &cmp));
        if (cmp >= 0) {
            cbt->compare = -1; /* page keys < search key */
            return (0);
        }
    }

    return (0);
}

/*
 * __wt_row_search --
 *     Search a row-store tree for a specific key.
 */
int
__wt_row_search(WT_CURSOR_BTREE *cbt, WT_ITEM *srch_key, bool insert, WT_REF *leaf, bool leaf_safe,
  bool *leaf_foundp)
{
    WT_BTREE *btree;
    WT_COLLATOR *collator;
    WT_DECL_RET;
    WT_INSERT_HEAD *ins_head;
    WT_ITEM *item;
    WT_PAGE *page;
    WT_PAGE_INDEX *pindex, *parent_pindex;
    WT_REF *current, *descent;
    WT_ROW *rip;
    WT_SESSION_IMPL *session;
    size_t match, skiphigh, skiplow;
    uint32_t base, indx, limit, read_flags;
    int cmp, depth;
    bool append_check, descend_right, done;

    session = CUR2S(cbt);
    btree = S2BT(session);
    collator = btree->collator;
    item = cbt->tmp;
    current = NULL;

    /*
     * Assert the session and cursor have the right relationship (not search specific, but search is
     * a convenient place to check given any operation on a cursor will likely search a page).
     */
    WT_ASSERT(session, session->dhandle == cbt->dhandle);

    __cursor_pos_clear(cbt);

    /*
     * In some cases we expect we're comparing more than a few keys with matching prefixes, so it's
     * faster to avoid the memory fetches by skipping over those prefixes. That's done by tracking
     * the length of the prefix match for the lowest and highest keys we compare as we descend the
     * tree. The high boundary is reset on each new page, the lower boundary is maintained.
     */
    skiplow = 0;

    /*
     * If a cursor repeatedly appends to the tree, compare the search key against the last key on
     * each internal page during insert before doing the full binary search.
     *
     * Track if the descent is to the right-side of the tree, used to set the cursor's append
     * history.
     */
    append_check = insert && cbt->append_tree;
    descend_right = true;

    /*
     * We may be searching only a single leaf page, not the full tree. In the normal case where we
     * are searching a tree, check the page's parent keys before doing the full search, it's faster
     * when the cursor is being re-positioned. Skip that check if we know the page is the right one
     * (for example, when re-instantiating a page in memory, in that case we know the target must be
     * on the current page).
     */
    if (leaf != NULL) {
        if (!leaf_safe) {
            WT_RET(__check_leaf_key_range(session, srch_key, leaf, cbt));
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
        skiplow = 0;
    }

    /* Search the internal pages of the tree. */
    current = &btree->root;
    for (depth = 2, pindex = NULL;; ++depth) {
        parent_pindex = pindex;
        page = current->page;
        if (page->type != WT_PAGE_ROW_INT)
            break;

        WT_INTL_INDEX_GET(session, page, pindex);

        /*
         * Fast-path appends.
         *
         * The 0th key on an internal page is a problem for a couple of reasons. First, we have to
         * force the 0th key to sort less than any application key, so internal pages don't have to
         * be updated if the application stores a new, "smallest" key in the tree. Second,
         * reconciliation is aware of this and will store a byte of garbage in the 0th key, so the
         * comparison of an application key and a 0th key is meaningless (but doing the comparison
         * could still incorrectly modify our tracking of the leading bytes in each key that we can
         * skip during the comparison). For these reasons, special-case the 0th key, and never pass
         * it to a collator.
         */
        if (append_check) {
            descent = pindex->index[pindex->entries - 1];

            if (pindex->entries == 1)
                goto append;
            __wt_ref_key(page, descent, &item->data, &item->size);
            WT_ERR(__wt_compare(session, collator, srch_key, item, &cmp));
            if (cmp >= 0)
                goto append;

            /* A failed append check turns off append checks. */
            append_check = false;
        }

        /*
         * Binary search of an internal page. There are three versions (keys with no
         * application-specified collation order, in long and short versions, and keys with an
         * application-specified collation order), because doing the tests and error handling inside
         * the loop costs about 5%.
         *
         * Reference the comment above about the 0th key: we continue to special-case it.
         */
        base = 1;
        limit = pindex->entries - 1;
        if (collator == NULL && srch_key->size <= WT_COMPARE_SHORT_MAXLEN)
            for (; limit != 0; limit >>= 1) {
                indx = base + (limit >> 1);
                descent = pindex->index[indx];
                __wt_ref_key(page, descent, &item->data, &item->size);

                cmp = __wt_lex_compare_short(srch_key, item);
                if (cmp > 0) {
                    base = indx + 1;
                    --limit;
                } else if (cmp == 0)
                    goto descend;
            }
        else if (collator == NULL) {
            /*
             * Reset the skipped prefix counts; we'd normally expect the parent's skipped prefix
             * values to be larger than the child's values and so we'd only increase them as we walk
             * down the tree (in other words, if we can skip N bytes on the parent, we can skip at
             * least N bytes on the child). However, if a child internal page was split up into the
             * parent, the child page's key space will have been truncated, and the values from the
             * parent's search may be wrong for the child. We only need to reset the high count
             * because the split-page algorithm truncates the end of the internal page's key space,
             * the low count is still correct.
             */
            skiphigh = 0;

            for (; limit != 0; limit >>= 1) {
                indx = base + (limit >> 1);
                descent = pindex->index[indx];
                __wt_ref_key(page, descent, &item->data, &item->size);

                match = WT_MIN(skiplow, skiphigh);
                cmp = __wt_lex_compare_skip(srch_key, item, &match);
                if (cmp > 0) {
                    skiplow = match;
                    base = indx + 1;
                    --limit;
                } else if (cmp < 0)
                    skiphigh = match;
                else
                    goto descend;
            }
        } else
            for (; limit != 0; limit >>= 1) {
                indx = base + (limit >> 1);
                descent = pindex->index[indx];
                __wt_ref_key(page, descent, &item->data, &item->size);

                WT_ERR(__wt_compare(session, collator, srch_key, item, &cmp));
                if (cmp > 0) {
                    base = indx + 1;
                    --limit;
                } else if (cmp == 0)
                    goto descend;
            }

        /*
         * Set the slot to descend the tree: descent was already set if there was an exact match on
         * the page, otherwise, base is the smallest index greater than key, possibly one past the
         * last slot.
         */
        descent = pindex->index[base - 1];

        /*
         * If we end up somewhere other than the last slot, it's not a right-side descent.
         */
        if (pindex->entries != base)
            descend_right = false;

        /*
         * If on the last slot (the key is larger than any key on the page), check for an internal
         * page split race.
         */
        if (pindex->entries == base) {
append:
            if (__wt_split_descent_race(session, current, parent_pindex))
                goto restart;
        }

descend:
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
     * Clear current now that we have moved the reference into the btree cursor, so that cleanup
     * never releases twice.
     */
    current = NULL;

    /*
     * In the case of a right-side tree descent during an insert, do a fast check for an append to
     * the page, try to catch cursors appending data into the tree.
     *
     * It's tempting to make this test more rigorous: if a cursor inserts randomly into a two-level
     * tree (a root referencing a single child that's empty except for an insert list), the
     * right-side descent flag will be set and this comparison wasted. The problem resolves itself
     * as the tree grows larger: either we're no longer doing right-side descent, or we'll avoid
     * additional comparisons in internal pages, making up for the wasted comparison here.
     * Similarly, the cursor's history is set any time it's an insert and a right-side descent, both
     * to avoid a complicated/expensive test, and, in the case of multiple threads appending to the
     * tree, we want to mark them all as appending, even if this test doesn't work.
     */
    if (insert && descend_right) {
        cbt->append_tree = 1;

        if (page->entries == 0) {
            cbt->slot = WT_ROW_SLOT(page, page->pg_row);

            F_SET(cbt, WT_CBT_SEARCH_SMALLEST);
            ins_head = WT_ROW_INSERT_SMALLEST(page);
        } else {
            cbt->slot = WT_ROW_SLOT(page, page->pg_row + (page->entries - 1));
            ins_head = WT_ROW_INSERT_SLOT(page, cbt->slot);
        }

        WT_ERR(__search_insert_append(session, cbt, ins_head, srch_key, &done));
        if (done)
            return (0);
    }

    /*
     * Binary search of an leaf page. There are three versions (keys with no application-specified
     * collation order, in long and short versions, and keys with an application-specified collation
     * order), because doing the tests and error handling inside the loop costs about 5%.
     */
    base = 0;
    limit = page->entries;
    if (collator == NULL && srch_key->size <= WT_COMPARE_SHORT_MAXLEN)
        for (; limit != 0; limit >>= 1) {
            indx = base + (limit >> 1);
            rip = page->pg_row + indx;
            WT_ERR(__wt_row_leaf_key(session, page, rip, item, true));

            cmp = __wt_lex_compare_short(srch_key, item);
            if (cmp > 0) {
                base = indx + 1;
                --limit;
            } else if (cmp == 0)
                goto leaf_match;
        }
    else if (collator == NULL) {
        /*
         * Reset the skipped prefix counts; we'd normally expect the parent's skipped prefix values
         * to be larger than the child's values and so we'd only increase them as we walk down the
         * tree (in other words, if we can skip N bytes on the parent, we can skip at least N bytes
         * on the child). However, leaf pages at the end of the tree can be extended, causing the
         * parent's search to be wrong for the child. We only need to reset the high count, the page
         * can only be extended so the low count is still correct.
         */
        skiphigh = 0;

        for (; limit != 0; limit >>= 1) {
            indx = base + (limit >> 1);
            rip = page->pg_row + indx;
            WT_ERR(__wt_row_leaf_key(session, page, rip, item, true));

            match = WT_MIN(skiplow, skiphigh);
            cmp = __wt_lex_compare_skip(srch_key, item, &match);
            if (cmp > 0) {
                skiplow = match;
                base = indx + 1;
                --limit;
            } else if (cmp < 0)
                skiphigh = match;
            else
                goto leaf_match;
        }
    } else
        for (; limit != 0; limit >>= 1) {
            indx = base + (limit >> 1);
            rip = page->pg_row + indx;
            WT_ERR(__wt_row_leaf_key(session, page, rip, item, true));

            WT_ERR(__wt_compare(session, collator, srch_key, item, &cmp));
            if (cmp > 0) {
                base = indx + 1;
                --limit;
            } else if (cmp == 0)
                goto leaf_match;
        }

    /*
     * The best case is finding an exact match in the leaf page's WT_ROW array, probable for any
     * read-mostly workload. Check that case and get out fast.
     */
    if (0) {
leaf_match:
        cbt->compare = 0;
        cbt->slot = WT_ROW_SLOT(page, rip);
        return (0);
    }

    /*
     * We didn't find an exact match in the WT_ROW array.
     *
     * Base is the smallest index greater than key and may be the 0th index or the (last + 1) index.
     * Set the slot to be the largest index less than the key if that's possible (if base is the 0th
     * index it means the application is inserting a key before any key found on the page).
     *
     * It's still possible there is an exact match, but it's on an insert list. Figure out which
     * insert chain to search and then set up the return information assuming we'll find nothing in
     * the insert list (we'll correct as needed inside the search routine, depending on what we
     * find).
     *
     * If inserting a key smaller than any key found in the WT_ROW array, use the extra slot of the
     * insert array, otherwise the insert array maps one-to-one to the WT_ROW array.
     */
    if (base == 0) {
        cbt->compare = 1;
        cbt->slot = 0;

        F_SET(cbt, WT_CBT_SEARCH_SMALLEST);
        ins_head = WT_ROW_INSERT_SMALLEST(page);
    } else {
        cbt->compare = -1;
        cbt->slot = base - 1;

        ins_head = WT_ROW_INSERT_SLOT(page, cbt->slot);
    }

    /* If there's no insert list, we're done. */
    if (WT_SKIP_FIRST(ins_head) == NULL)
        return (0);

    /*
     * Test for an append first when inserting onto an insert list, try to catch cursors repeatedly
     * inserting at a single point, then search the insert list. If we find an exact match, copy the
     * key into the temporary buffer, our callers expect to find it there.
     */
    if (insert) {
        WT_ERR(__search_insert_append(session, cbt, ins_head, srch_key, &done));
        if (done)
            return (0);
    }
    WT_ERR(__wt_search_insert(session, cbt, ins_head, srch_key));
    if (cbt->compare == 0) {
        cbt->tmp->data = WT_INSERT_KEY(cbt->ins);
        cbt->tmp->size = WT_INSERT_KEY_SIZE(cbt->ins);
    }
    return (0);

err:
    WT_TRET(__wt_page_release(session, current, 0));
    return (ret);
}
