/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __random_insert_valid --
 *     Check if the inserted key/value pair is valid.
 */
static int
__random_insert_valid(WT_CURSOR_BTREE *cbt, WT_INSERT_HEAD *ins_head, WT_INSERT *ins, bool *validp)
{
    *validp = false;

    __cursor_pos_clear(cbt);
    cbt->slot = 0;
    cbt->ins_head = ins_head;
    cbt->ins = ins;
    cbt->compare = 0;
    cbt->tmp->data = WT_INSERT_KEY(ins);
    cbt->tmp->size = WT_INSERT_KEY_SIZE(ins);

    return (__wt_cursor_valid(cbt, validp, false));
}

/*
 * __random_slot_valid --
 *     Check if the slot key/value pair is valid.
 */
static int
__random_slot_valid(WT_CURSOR_BTREE *cbt, uint32_t slot, bool *validp)
{
    *validp = false;

    __cursor_pos_clear(cbt);
    cbt->slot = slot;
    cbt->compare = 0;

    return (__wt_cursor_valid(cbt, validp, false));
}

/* Magic constant: 5000 entries in a skip list is enough to forcibly evict. */
#define WT_RANDOM_SKIP_EVICT_SOON (5 * WT_THOUSAND)
/* Magic constant: 50 entries in a skip list is enough to predict the size. */
#define WT_RANDOM_SKIP_PREDICT 50

/*
 * __random_skip_entries --
 *     Return an estimate of how many entries are in a skip list.
 */
static uint32_t
__random_skip_entries(WT_CURSOR_BTREE *cbt, WT_INSERT_HEAD *ins_head)
{
    WT_INSERT **t;
    WT_SESSION_IMPL *session;
    uint32_t entries;
    int level;

    session = CUR2S(cbt);
    entries = 0; /* [-Wconditional-uninitialized] */

    if (ins_head == NULL)
        return (0);

    /* Find a level with enough entries on it to predict the size of the list. */
    for (level = WT_SKIP_MAXDEPTH - 1; level >= 0; --level) {
        for (entries = 0, t = &ins_head->head[level]; *t != NULL; t = &(*t)->next[level])
            ++entries;

        if (entries > WT_RANDOM_SKIP_PREDICT)
            break;
    }

    /* Use the skiplist probability to estimate the size of the list. */
    WT_ASSERT(session, WT_SKIP_PROBABILITY == UINT32_MAX >> 2);
    while (--level >= 0)
        entries *= 4;

    /*
     * Random lookups in newly created collections can be slow if a page consists of a large
     * skiplist. Schedule the page for eviction if we encounter a large skiplist. This is worthwhile
     * because applications that take a sample often take many samples, so the overhead of
     * traversing the skip list each time accumulates to real time.
     */
    if (entries > WT_RANDOM_SKIP_EVICT_SOON)
        __wt_page_evict_soon(session, cbt->ref);

    return (entries);
}

/* Magic constant: check 3 records before/after the selected record. */
#define WT_RANDOM_SKIP_LOCAL 3
/* Magic constant: retry 3 times in a skip list before giving up. */
#define WT_RANDOM_SKIP_RETRY 3

/*
 * __random_leaf_skip --
 *     Return a random key/value from a skip list.
 */
static int
__random_leaf_skip(WT_CURSOR_BTREE *cbt, WT_INSERT_HEAD *ins_head, uint32_t entries, bool *validp)
{
    WT_INSERT *ins, *saved_ins;
    WT_SESSION_IMPL *session;
    uint32_t i;
    int retry;

    *validp = false;

    session = CUR2S(cbt);

    /* This is a relatively expensive test, try a few times then quit. */
    for (retry = 0; retry < WT_RANDOM_SKIP_RETRY; ++retry) {
        /*
         * Randomly select a record in the skip list and walk to it. Remember the entry a few
         * records before our target so we can look around in case our chosen record isn't valid.
         */
        saved_ins = NULL;
        i = __wt_random(&session->rnd) % entries;
        for (ins = WT_SKIP_FIRST(ins_head); ins != NULL; ins = WT_SKIP_NEXT(ins)) {
            if (--i == 0)
                break;
            if (i == WT_RANDOM_SKIP_LOCAL * 2)
                saved_ins = ins;
        }

        /* Try and return our selected record. */
        if (ins != NULL) {
            WT_RET(__random_insert_valid(cbt, ins_head, ins, validp));
            if (*validp)
                return (0);
        }

        /* Check a few records before/after our selected record. */
        i = WT_RANDOM_SKIP_LOCAL;
        if (saved_ins != NULL) {
            i = WT_RANDOM_SKIP_LOCAL * 2;
            ins = saved_ins;
        }
        for (; --i > 0 && ins != NULL; ins = WT_SKIP_NEXT(ins)) {
            WT_RET(__random_insert_valid(cbt, ins_head, ins, validp));
            if (*validp)
                return (0);
        }
    }
    return (0);
}

/* Magic constant: 100 entries in any randomly chosen skip list is enough to select from it. */
#define WT_RANDOM_SKIP_INSERT_ENOUGH 100
/* Magic constant: 1000 entries in an initial skip list is enough to always select from it. */
#define WT_RANDOM_SKIP_INSERT_SMALLEST_ENOUGH WT_THOUSAND

/*
 * __random_leaf_insert --
 *     Look for a large insert list from which we can select a random item.
 */
static int
__random_leaf_insert(WT_CURSOR_BTREE *cbt, bool *validp)
{
    WT_INSERT_HEAD *ins_head;
    WT_PAGE *page;
    WT_SESSION_IMPL *session;
    uint32_t entries, slot, start;

    *validp = false;

    page = cbt->ref->page;
    session = CUR2S(cbt);

    /* Check for a large insert list with no items, that's common when tables are newly created. */
    ins_head = WT_ROW_INSERT_SMALLEST(page);
    entries = __random_skip_entries(cbt, ins_head);
    if (entries >= WT_RANDOM_SKIP_INSERT_SMALLEST_ENOUGH) {
        WT_RET(__random_leaf_skip(cbt, ins_head, entries, validp));
        if (*validp)
            return (0);
    }

    /*
     * Look for any reasonably large insert list. We're selecting a random insert list and won't end
     * up on the same insert list every time we search this page (unless there's only one list), so
     * decrease the required number of records required to select from the list.
     */
    if (page->entries > 0) {
        start = __wt_random(&session->rnd) % page->entries;
        for (slot = start; slot < page->entries; ++slot) {
            ins_head = WT_ROW_INSERT(page, &page->pg_row[slot]);
            entries = __random_skip_entries(cbt, ins_head);
            if (entries >= WT_RANDOM_SKIP_INSERT_ENOUGH) {
                WT_RET(__random_leaf_skip(cbt, ins_head, entries, validp));
                if (*validp)
                    return (0);
            }
        }
        for (slot = 0; slot < start; ++slot) {
            ins_head = WT_ROW_INSERT(page, &page->pg_row[slot]);
            entries = __random_skip_entries(cbt, ins_head);
            if (entries >= WT_RANDOM_SKIP_INSERT_ENOUGH) {
                WT_RET(__random_leaf_skip(cbt, ins_head, entries, validp));
                if (*validp)
                    return (0);
            }
        }
    }

    /* Fall back to the single insert list, if it's not tiny. */
    ins_head = WT_ROW_INSERT_SMALLEST(page);
    entries = __random_skip_entries(cbt, ins_head);
    if (entries >= WT_RANDOM_SKIP_INSERT_ENOUGH) {
        WT_RET(__random_leaf_skip(cbt, ins_head, entries, validp));
        if (*validp)
            return (0);
    }
    return (0);
}

/* Magic constant: retry 10 times in the disk-based entries before giving up. */
#define WT_RANDOM_DISK_RETRY 10

/*
 * __random_leaf_disk --
 *     Return a random key/value from a page's on-disk entries.
 */
static int
__random_leaf_disk(WT_CURSOR_BTREE *cbt, bool *validp)
{
    WT_PAGE *page;
    WT_SESSION_IMPL *session;
    uint32_t entries, slot;
    int retry;

    *validp = false;

    page = cbt->ref->page;
    session = CUR2S(cbt);
    entries = cbt->ref->page->entries;

    /* This is a relatively cheap test, so try several times. */
    for (retry = 0; retry < WT_RANDOM_DISK_RETRY; ++retry) {
        slot = __wt_random(&session->rnd) % entries;
        WT_RET(__wt_row_leaf_key(session, page, page->pg_row + slot, cbt->tmp, false));
        WT_RET(__random_slot_valid(cbt, slot, validp));
        if (*validp)
            break;
    }
    return (0);
}

/* Magic constant: cursor up to 250 next/previous records before selecting a key. */
#define WT_RANDOM_CURSOR_MOVE 250
/* Magic constant: 1000 disk-based entries in a page is enough to always select from them. */
#define WT_RANDOM_DISK_ENOUGH WT_THOUSAND

/*
 * __random_leaf --
 *     Return a random key/value from a row-store leaf page.
 */
static int
__random_leaf(WT_CURSOR_BTREE *cbt)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    uint32_t i;
    bool next, valid;

    cursor = &cbt->iface;
    session = CUR2S(cbt);

    /*
     * If the page has a sufficiently large number of disk-based entries, randomly select from them.
     * Ignoring large insert lists could skew the results, but enough disk-based entries should span
     * a reasonable chunk of the name space.
     */
    if (cbt->ref->page->entries > WT_RANDOM_DISK_ENOUGH) {
        WT_RET(__random_leaf_disk(cbt, &valid));
        if (valid)
            return (__cursor_kv_return(cbt, cbt->upd_value));
    }

    /* Look for any large insert list and select from it. */
    WT_RET(__random_leaf_insert(cbt, &valid));
    if (valid)
        return (__cursor_kv_return(cbt, cbt->upd_value));

    /*
     * Try again if there are at least a few hundred disk-based entries or this is a page as we read
     * it from disk, it might be a normal leaf page with big items.
     */
    if (cbt->ref->page->entries > WT_RANDOM_DISK_ENOUGH / 5 ||
      (cbt->ref->page->dsk != NULL && cbt->ref->page->modify == NULL)) {
        WT_RET(__random_leaf_disk(cbt, &valid));
        if (valid)
            return (__cursor_kv_return(cbt, cbt->upd_value));
    }

    /*
     * We don't have many disk-based entries, we didn't find any large insert lists. Where we get
     * into trouble is a small number of pages with large numbers of deleted items. Try and move out
     * of the problematic namespace into something we can use by cursoring forward or backward. On a
     * page with a sufficiently large group of deleted items where the randomly selected entries are
     * all deleted, simply moving to the next or previous record likely means moving to the same
     * record every time, so move the cursor a random number of items. Further, detect if we're
     * about to return the same item twice in a row and try to avoid it. (If there's only a single
     * record, or only a pair of records, we'll still end up in trouble, but at some point the tree
     * is too small to do anything better.) All of this is slow and expensive, but the alternative
     * is customer complaints.
     */
    __cursor_pos_clear(cbt);
    cbt->slot = 0;
    next = true; /* Forward from the beginning of the page. */
    for (i = __wt_random(&session->rnd) % WT_RANDOM_CURSOR_MOVE;;) {
        ret = next ? __wt_btcur_next(cbt, false) : __wt_btcur_prev(cbt, false);
        if (ret == WT_NOTFOUND) {
            next = !next; /* Reverse direction. */
            ret = next ? __wt_btcur_next(cbt, false) : __wt_btcur_prev(cbt, false);
        }
        WT_RET(ret);

        if (i > 0)
            --i;
        else {
            /*
             * Skip the record we returned last time, once. Clear the tracking value so we don't
             * skip that record twice, it just means the tree is too small for anything reasonable.
             *
             * Testing WT_DATA_IN_ITEM requires explanation: the cursor temporary buffer is used to
             * build keys for row-store searches and can point into the row-store page (which might
             * have been freed subsequently). If a previous random call set the temporary buffer,
             * then it will be local data. If it's local data for some other reason than a previous
             * random call, we don't care: it won't match, and if it does we just retry.
             */
            if (WT_DATA_IN_ITEM(cbt->tmp) && cursor->key.size == cbt->tmp->size &&
              memcmp(cursor->key.data, cbt->tmp->data, cbt->tmp->size) == 0) {
                cbt->tmp->size = 0;
                i = __wt_random(&session->rnd) % WT_RANDOM_CURSOR_MOVE;
            } else {
                WT_RET(__wt_buf_set(session, cbt->tmp, cursor->key.data, cursor->key.size));
                break;
            }
        }
    }

    return (0);
}

/*
 * __wt_random_descent --
 *     Find a random page in a tree for either sampling or eviction.
 */
int
__wt_random_descent(WT_SESSION_IMPL *session, WT_REF **refp, uint32_t flags)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_PAGE *page;
    WT_PAGE_INDEX *pindex;
    WT_REF *current, *descent;
    uint32_t i, entries, retry;
    bool eviction;

    *refp = NULL;

    btree = S2BT(session);
    current = NULL;
    retry = 100;
    /*
     * This function is called by eviction to find a random page in the cache. That case is
     * indicated by the WT_READ_CACHE flag. Ordinary lookups in a tree will read pages into cache as
     * needed.
     */
    eviction = LF_ISSET(WT_READ_CACHE);

    if (0) {
restart:
        /*
         * Discard the currently held page and restart the search from the root.
         */
        WT_RET(__wt_page_release(session, current, flags));
    }

    /* Search the internal pages of the tree. */
    current = &btree->root;
    for (;;) {
        if (F_ISSET(current, WT_REF_FLAG_LEAF))
            break;

        page = current->page;
        WT_INTL_INDEX_GET(session, page, pindex);
        entries = pindex->entries;

        /* Eviction just wants any random child. */
        if (eviction) {
            descent = pindex->index[__wt_random(&session->rnd) % entries];
            goto descend;
        }

        /*
         * There may be empty pages in the tree, and they're useless to us. If we don't find a
         * non-empty page in "entries" random guesses, take the first non-empty page in the tree. If
         * the search page contains nothing other than empty pages, restart from the root some
         * number of times before giving up.
         *
         * Random sampling is looking for a key/value pair on a random leaf page, and so will accept
         * any page that contains a valid key/value pair, so on-disk is fine, but deleted is not.
         */
        descent = NULL;
        for (i = 0; i < entries; ++i) {
            descent = pindex->index[__wt_random(&session->rnd) % entries];
            if (descent->state == WT_REF_DISK || descent->state == WT_REF_MEM)
                break;
        }
        if (i == entries)
            for (i = 0; i < entries; ++i) {
                descent = pindex->index[i];
                if (descent->state == WT_REF_DISK || descent->state == WT_REF_MEM)
                    break;
            }
        if (i == entries || descent == NULL) {
            if (--retry > 0)
                goto restart;

            WT_RET(__wt_page_release(session, current, flags));
            return (WT_NOTFOUND);
        }

        /*
         * Swap the current page for the child page. If the page splits while we're retrieving it,
         * restart the search at the root.
         *
         * On other error, simply return, the swap call ensures we're holding nothing on failure.
         */
descend:
        if ((ret = __wt_page_swap(session, current, descent, flags)) == 0) {
            current = descent;
            continue;
        }
        if (eviction && (ret == WT_NOTFOUND || ret == WT_RESTART))
            break;
        if (ret == WT_RESTART)
            goto restart;
        return (ret);
    }

    /*
     * There is no point starting with the root page: the walk will exit immediately. In that case
     * we aren't holding a hazard pointer so there is nothing to release.
     */
    if (!eviction || !__wt_ref_is_root(current))
        *refp = current;
    return (0);
}

/*
 * __wt_btcur_next_random --
 *     Move to a random record in the tree. There are two algorithms, one where we select a record
 *     at random from the whole tree on each retrieval and one where we first select a record at
 *     random from the whole tree, and then subsequently sample forward from that location. The
 *     sampling approach allows us to select reasonably uniform random points from unbalanced trees.
 */
int
__wt_btcur_next_random(WT_CURSOR_BTREE *cbt)
{
    WT_BTREE *btree;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    wt_off_t size;
    uint64_t n, skip;
    uint32_t read_flags;

    btree = CUR2BT(cbt);
    cursor = &cbt->iface;
    session = CUR2S(cbt);

    read_flags = WT_READ_RESTART_OK;
    if (F_ISSET(cbt, WT_CBT_READ_ONCE))
        FLD_SET(read_flags, WT_READ_WONT_NEED);

    /*
     * Only supports row-store: applications can trivially select a random value from a
     * column-store, if there were any reason to do so.
     */
    if (btree->type != BTREE_ROW)
        WT_RET_MSG(session, ENOTSUP, "WT_CURSOR.next_random only supported by row-store tables");

    WT_STAT_CONN_DATA_INCR(session, cursor_next);

    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

#ifdef HAVE_DIAGNOSTIC
    /*
     * Under some conditions we end up using the underlying cursor.next to walk through the object.
     * Since there are multiple calls, we can hit the cursor-order checks, turn them off.
     */
    __wt_cursor_key_order_reset(cbt);
#endif
    /*
     * If we don't have a current position in the tree, or if retrieving random values without
     * sampling, pick a roughly random leaf page in the tree and return an entry from it.
     */
    if (cbt->ref == NULL || cbt->next_random_sample_size == 0) {
        WT_ERR(__wt_cursor_func_init(cbt, true));
        WT_WITH_PAGE_INDEX(session, ret = __wt_random_descent(session, &cbt->ref, read_flags));
        if (ret == 0) {
            WT_ERR(__random_leaf(cbt));
            return (0);
        }

        /*
         * Random descent may return not-found: the tree might be empty or have so many deleted
         * items we didn't find any valid pages. We can't return WT_NOTFOUND to the application
         * unless a tree is really empty, fallback to skipping through tree pages.
         */
        WT_ERR_NOTFOUND_OK(ret, false);
    }

    /*
     * Cursor through the tree, skipping past the sample size of the leaf pages in the tree between
     * each random key return to compensate for unbalanced trees.
     *
     * If the random descent attempt failed, we don't have a configured sample size, use 100 for no
     * particular reason.
     */
    if (cbt->next_random_sample_size == 0)
        cbt->next_random_sample_size = 100;

    /*
     * If the random descent attempt failed, or it's our first skip attempt,
     * we haven't yet set the pages to skip, do it now.
     *
     * Use the underlying file size divided by its block allocation size as
     * our guess of leaf pages in the file (this can be entirely wrong, as
     * it depends on how many pages are in this particular checkpoint, how
     * large the leaf and internal pages really are, and other factors).
     * Then, divide that value by the configured sample size and increment
     * the final result to make sure tiny files don't leave us with a skip
     * value of 0.
     *
     * !!!
     * Ideally, the number would be prime to avoid restart issues.
     */
    if (cbt->next_random_leaf_skip == 0) {
        WT_ERR(btree->bm->size(btree->bm, session, &size));
        cbt->next_random_leaf_skip =
          (uint64_t)((size / btree->allocsize) / cbt->next_random_sample_size) + 1;
    }

    /*
     * Be paranoid about loop termination: first, if the last leaf page skipped was also the last
     * leaf page in the tree, skip may be set to zero on return along with the NULL WT_REF
     * end-of-walk condition. Second, if a tree has no valid pages at all (the condition after
     * initial creation), we might make no progress at all, or finally, if a tree has only deleted
     * pages, we'll make progress, but never get a useful WT_REF. And, of course, the tree can
     * switch from one of these states to another without warning. Decrement skip regardless of what
     * is happening in the search, guarantee we eventually quit.
     *
     * Pages read for data sampling aren't "useful"; don't update the read generation of pages
     * already in memory, and if a page is read, set its generation to a low value so it is evicted
     * quickly.
     */
    for (skip = cbt->next_random_leaf_skip; cbt->ref == NULL || skip > 0;) {
        n = skip;
        WT_ERR(__wt_tree_walk_skip(session, &cbt->ref, &skip));
        if (n == skip) {
            if (skip == 0)
                break;
            --skip;
        }
    }

    /*
     * We can't return WT_NOTFOUND to the application unless a tree is really empty, fallback to a
     * random entry from the first page in the tree that has anything at all.
     */
    if (cbt->ref == NULL)
        WT_ERR(__wt_btcur_next(cbt, false));

    /* Select a random entry from the leaf page. */
    WT_ERR(__random_leaf(cbt));

    return (0);

err:
    WT_TRET(__cursor_reset(cbt));
    return (ret);
}
