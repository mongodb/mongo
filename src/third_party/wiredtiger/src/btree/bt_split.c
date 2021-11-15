/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#define WT_MEM_TRANSFER(from_decr, to_incr, len) \
    do {                                         \
        size_t __len = (len);                    \
        (from_decr) += __len;                    \
        (to_incr) += __len;                      \
    } while (0)

/*
 * A note on error handling: main split functions first allocate/initialize new structures; failures
 * during that period are handled by discarding the memory and returning an error code, the caller
 * knows the split didn't happen and proceeds accordingly. Second, split functions update the tree,
 * and a failure in that period is catastrophic, any partial update to the tree requires a panic, we
 * can't recover. Third, once the split is complete and the tree has been fully updated, we have to
 * ignore most errors, the split is complete and correct, callers have to proceed accordingly.
 */
typedef enum {
    WT_ERR_IGNORE, /* Ignore minor errors */
    WT_ERR_PANIC,  /* Panic on all errors */
    WT_ERR_RETURN  /* Clean up and return error */
} WT_SPLIT_ERROR_PHASE;

/*
 * __split_safe_free --
 *     Free a buffer if we can be sure no thread is accessing it, or schedule it to be freed
 *     otherwise.
 */
static int
__split_safe_free(WT_SESSION_IMPL *session, uint64_t split_gen, bool exclusive, void *p, size_t s)
{
    /*
     * We have swapped something in a page: it's only safe to free it if we have exclusive access.
     */
    if (exclusive) {
        __wt_overwrite_and_free_len(session, p, s);
        return (0);
    }

    return (__wt_stash_add(session, WT_GEN_SPLIT, split_gen, p, s));
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __split_verify_intl_key_order --
 *     Verify the key order on an internal page after a split.
 */
static void
__split_verify_intl_key_order(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_BTREE *btree;
    WT_ITEM *next, _next, *last, _last, *tmp;
    WT_REF *ref;
    uint64_t recno;
    uint32_t slot;
    int cmp;

    btree = S2BT(session);

    switch (page->type) {
    case WT_PAGE_COL_INT:
        recno = 0; /* Less than any valid record number. */
        WT_INTL_FOREACH_BEGIN (session, page, ref) {
            WT_ASSERT(session, ref->home == page);

            WT_ASSERT(session, ref->ref_recno > recno);
            recno = ref->ref_recno;
        }
        WT_INTL_FOREACH_END;
        break;
    case WT_PAGE_ROW_INT:
        next = &_next;
        WT_CLEAR(_next);
        last = &_last;
        WT_CLEAR(_last);

        slot = 0;
        WT_INTL_FOREACH_BEGIN (session, page, ref) {
            WT_ASSERT(session, ref->home == page);

            /*
             * Don't compare the first slot with any other slot, it's ignored on row-store internal
             * pages.
             */
            __wt_ref_key(page, ref, &next->data, &next->size);
            if (++slot > 2) {
                WT_ASSERT(session, __wt_compare(session, btree->collator, last, next, &cmp) == 0);
                WT_ASSERT(session, cmp < 0);
            }
            tmp = last;
            last = next;
            next = tmp;
        }
        WT_INTL_FOREACH_END;
        break;
    }
}

/*
 * __split_verify_root --
 *     Verify a root page involved in a split.
 */
static int
__split_verify_root(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_DECL_RET;
    WT_REF *ref;
    uint32_t read_flags;

    /*
     * Ignore pages not in-memory (deleted, on-disk, being read), there's no in-memory structure to
     * check.
     */
    read_flags = WT_READ_CACHE | WT_READ_NO_EVICT;

    /* The split is complete and live, verify all of the pages involved. */
    __split_verify_intl_key_order(session, page);

    WT_INTL_FOREACH_BEGIN (session, page, ref) {
        /*
         * The page might be in transition, being read or evicted or something else. Acquire a
         * hazard pointer for the page so we know its state.
         */
        if ((ret = __wt_page_in(session, ref, read_flags)) == WT_NOTFOUND)
            continue;
        WT_ERR(ret);

        __split_verify_intl_key_order(session, ref->page);

        WT_ERR(__wt_page_release(session, ref, read_flags));
    }
    WT_INTL_FOREACH_END;

    return (0);

err:
    /* Something really bad just happened. */
    WT_RET_PANIC(session, ret, "fatal error during page split");
}
#endif

/*
 * __split_ovfl_key_cleanup --
 *     Handle cleanup for on-page row-store overflow keys.
 */
static int
__split_ovfl_key_cleanup(WT_SESSION_IMPL *session, WT_PAGE *page, WT_REF *ref)
{
    WT_CELL *cell;
    WT_CELL_UNPACK_KV kpack;
    WT_IKEY *ikey;
    uint32_t cell_offset;

    /* There's a per-page flag if there are any overflow keys at all. */
    if (!F_ISSET_ATOMIC(page, WT_PAGE_INTL_OVERFLOW_KEYS))
        return (0);

    /*
     * A key being discarded (page split) or moved to a different page (page deepening) may be an
     * on-page overflow key. Clear any reference to an underlying disk image, and, if the key hasn't
     * been deleted, delete it along with any backing blocks.
     */
    if ((ikey = __wt_ref_key_instantiated(ref)) == NULL)
        return (0);
    if ((cell_offset = ikey->cell_offset) == 0)
        return (0);

    /* Leak blocks rather than try this twice. */
    ikey->cell_offset = 0;

    cell = WT_PAGE_REF_OFFSET(page, cell_offset);
    __wt_cell_unpack_kv(session, page->dsk, cell, &kpack);
    if (FLD_ISSET(kpack.flags, WT_CELL_UNPACK_OVERFLOW) && kpack.raw != WT_CELL_KEY_OVFL_RM)
        WT_RET(__wt_ovfl_discard(session, page, cell));

    return (0);
}

/*
 * __split_ref_move --
 *     Move a WT_REF from one page to another, including updating accounting information.
 */
static int
__split_ref_move(WT_SESSION_IMPL *session, WT_PAGE *from_home, WT_REF **from_refp, size_t *decrp,
  WT_REF **to_refp, size_t *incrp)
{
    WT_ADDR *addr, *ref_addr;
    WT_CELL_UNPACK_ADDR unpack;
    WT_DECL_RET;
    WT_IKEY *ikey;
    WT_REF *ref;
    size_t size;
    void *key;

    ref = *from_refp;
    addr = NULL;

    /*
     * The from-home argument is the page into which the "from" WT_REF may point, for example, if
     * there's an on-page key the "from" WT_REF references, it will be on the page "from-home".
     *
     * Instantiate row-store keys, and column- and row-store addresses in the WT_REF structures
     * referenced by a page that's being split. The WT_REF structures aren't moving, but the index
     * references are moving from the page we're splitting to a set of new pages, and so we can no
     * longer reference the block image that remains with the page being split.
     *
     * No locking is required to update the WT_REF structure because we're the only thread splitting
     * the page, and there's no way for readers to race with our updates of single pointers. The
     * changes have to be written before the page goes away, of course, our caller owns that
     * problem.
     */
    if (from_home->type == WT_PAGE_ROW_INT) {
        /*
         * Row-store keys: if it's not yet instantiated, instantiate it. If already instantiated,
         * check for overflow cleanup (overflow keys are always instantiated).
         */
        if ((ikey = __wt_ref_key_instantiated(ref)) == NULL) {
            __wt_ref_key(from_home, ref, &key, &size);
            WT_RET(__wt_row_ikey(session, 0, key, size, ref));
            ikey = ref->ref_ikey;
        } else {
            WT_RET(__split_ovfl_key_cleanup(session, from_home, ref));
            *decrp += sizeof(WT_IKEY) + ikey->size;
        }
        *incrp += sizeof(WT_IKEY) + ikey->size;
    }

    /*
     * If there's no address at all (the page has never been written), or the address has already
     * been instantiated, there's no work to do. Otherwise, the address still references a split
     * page on-page cell, instantiate it. We can race with reconciliation and/or eviction of the
     * child pages, be cautious: read the address and verify it, and only update it if the value is
     * unchanged from the original. In the case of a race, the address must no longer reference the
     * split page, we're done.
     */
    WT_ORDERED_READ(ref_addr, ref->addr);
    if (ref_addr != NULL && !__wt_off_page(from_home, ref_addr)) {
        __wt_cell_unpack_addr(session, from_home->dsk, (WT_CELL *)ref_addr, &unpack);
        WT_RET(__wt_calloc_one(session, &addr));
        WT_TIME_AGGREGATE_COPY(&addr->ta, &unpack.ta);
        WT_ERR(__wt_memdup(session, unpack.data, unpack.size, &addr->addr));
        addr->size = (uint8_t)unpack.size;
        switch (unpack.raw) {
        case WT_CELL_ADDR_INT:
            addr->type = WT_ADDR_INT;
            break;
        case WT_CELL_ADDR_LEAF:
            addr->type = WT_ADDR_LEAF;
            break;
        case WT_CELL_ADDR_LEAF_NO:
            addr->type = WT_ADDR_LEAF_NO;
            break;
        default:
            WT_ERR(__wt_illegal_value(session, unpack.raw));
        }
        /* If the compare-and-swap is successful, clear addr to skip the free at the end. */
        if (__wt_atomic_cas_ptr(&ref->addr, ref_addr, addr))
            addr = NULL;
    }

    /* And finally, copy the WT_REF pointer itself. */
    *to_refp = ref;
    WT_MEM_TRANSFER(*decrp, *incrp, sizeof(WT_REF));

err:
    if (addr != NULL) {
        __wt_free(session, addr->addr);
        __wt_free(session, addr);
    }
    return (ret);
}

/*
 * __split_ref_final --
 *     Finalize the WT_REF move.
 */
static void
__split_ref_final(WT_SESSION_IMPL *session, uint64_t split_gen, WT_PAGE ***lockedp)
{
    WT_PAGE **locked;
    size_t i;

    /* The parent page's page index has been updated. */
    WT_WRITE_BARRIER();

    if ((locked = *lockedp) == NULL)
        return;
    *lockedp = NULL;

    /*
     * The moved child pages are locked to prevent them from splitting before the parent move
     * completes, unlock them as the final step.
     *
     * Once the split is live, newly created internal pages might be evicted and their WT_REF
     * structures freed. If that happens before all threads exit the index of the page that
     * previously "owned" the WT_REF, a thread might see a freed WT_REF. To ensure that doesn't
     * happen, the created pages are set to the current split generation and so can't be evicted
     * until all readers have left the old generation.
     */
    for (i = 0; locked[i] != NULL; ++i) {
        if (split_gen != 0 && WT_PAGE_IS_INTERNAL(locked[i]))
            locked[i]->pg_intl_split_gen = split_gen;
        WT_PAGE_UNLOCK(session, locked[i]);
    }
    __wt_free(session, locked);
}

/*
 * __split_ref_prepare --
 *     Prepare a set of WT_REFs for a move.
 */
static int
__split_ref_prepare(
  WT_SESSION_IMPL *session, WT_PAGE_INDEX *pindex, WT_PAGE ***lockedp, bool skip_first)
{
    WT_DECL_RET;
    WT_PAGE *child, **locked;
    WT_REF *child_ref, *ref;
    size_t alloc, cnt;
    uint32_t i, j;

    *lockedp = NULL;

    locked = NULL;

    /*
     * Update the moved WT_REFs so threads moving through them start looking at the created
     * children's page index information. Because we've not yet updated the page index of the parent
     * page into which we are going to split this subtree, a cursor moving through these WT_REFs
     * will ascend into the created children, but eventually fail as that parent page won't yet know
     * about the created children pages. That's OK, we spin there until the parent's page index is
     * updated.
     *
     * Lock the newly created page to ensure none of its children can split. First, to ensure all of
     * the child pages are updated before any pages can split. Second, to ensure the original split
     * completes before any of the children can split. The latter involves split generations: the
     * original split page has references to these children. If they split immediately, they could
     * free WT_REF structures based on split generations earlier than the split generation we'll
     * eventually choose to protect the original split page's previous page index.
     */
    alloc = cnt = 0;
    for (i = skip_first ? 1 : 0; i < pindex->entries; ++i) {
        ref = pindex->index[i];
        child = ref->page;

        /* Track the locked pages for cleanup. */
        WT_ERR(__wt_realloc_def(session, &alloc, cnt + 2, &locked));
        locked[cnt++] = child;

        WT_PAGE_LOCK(session, child);

        /* Switch the WT_REF's to their new page. */
        j = 0;
        WT_INTL_FOREACH_BEGIN (session, child, child_ref) {
            child_ref->home = child;
            child_ref->pindex_hint = j++;
        }
        WT_INTL_FOREACH_END;

#ifdef HAVE_DIAGNOSTIC
        WT_WITH_PAGE_INDEX(session, __split_verify_intl_key_order(session, child));
#endif
    }
    *lockedp = locked;
    return (0);

err:
    __split_ref_final(session, 0, &locked);
    return (ret);
}

/*
 * __split_root --
 *     Split the root page in-memory, deepening the tree.
 */
static int
__split_root(WT_SESSION_IMPL *session, WT_PAGE *root)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_PAGE *child, **locked;
    WT_PAGE_INDEX *alloc_index, *child_pindex, *pindex;
    WT_REF **alloc_refp, **child_refp, *ref, **root_refp;
    WT_SPLIT_ERROR_PHASE complete;
    size_t child_incr, root_decr, root_incr, size;
    uint64_t split_gen;
    uint32_t children, chunk, i, j, remain;
    uint32_t slots;
    void *p;

    WT_STAT_CONN_DATA_INCR(session, cache_eviction_deepen);
    WT_STAT_CONN_DATA_INCR(session, cache_eviction_split_internal);

    btree = S2BT(session);
    alloc_index = NULL;
    locked = NULL;
    root_decr = root_incr = 0;
    complete = WT_ERR_RETURN;

    /* Mark the root page dirty. */
    WT_RET(__wt_page_modify_init(session, root));
    __wt_page_modify_set(session, root);

    /*
     * Our caller is holding the root page locked to single-thread splits, which means we can safely
     * look at the page's index without setting a split generation.
     */
    pindex = WT_INTL_INDEX_GET_SAFE(root);

    /*
     * Decide how many child pages to create, then calculate the standard chunk and whatever
     * remains. Sanity check the number of children: the decision to split matched to the
     * deepen-per-child configuration might get it wrong.
     */
    children = pindex->entries / btree->split_deepen_per_child;
    if (children < 10) {
        if (pindex->entries < 100)
            return (__wt_set_return(session, EBUSY));
        children = 10;
    }
    chunk = pindex->entries / children;
    remain = pindex->entries - chunk * (children - 1);

    __wt_verbose(session, WT_VERB_SPLIT,
      "%p: %" PRIu32 " root page elements, splitting into %" PRIu32 " children", (void *)root,
      pindex->entries, children);

    /*
     * Allocate a new WT_PAGE_INDEX and set of WT_REF objects to be inserted into the root page,
     * replacing the root's page-index.
     */
    size = sizeof(WT_PAGE_INDEX) + children * sizeof(WT_REF *);
    WT_ERR(__wt_calloc(session, 1, size, &alloc_index));
    root_incr += size;
    alloc_index->index = (WT_REF **)(alloc_index + 1);
    alloc_index->entries = children;
    alloc_refp = alloc_index->index;
    for (i = 0; i < children; alloc_refp++, ++i)
        WT_ERR(__wt_calloc_one(session, alloc_refp));
    root_incr += children * sizeof(WT_REF);

    /* Allocate child pages, and connect them into the new page index. */
    for (root_refp = pindex->index, alloc_refp = alloc_index->index, i = 0; i < children; ++i) {
        slots = i == children - 1 ? remain : chunk;

        WT_ERR(__wt_page_alloc(session, root->type, slots, false, &child));

        /*
         * Initialize the page's child reference; we need a copy of the page's key.
         */
        ref = *alloc_refp++;
        ref->home = root;
        ref->page = child;
        ref->addr = NULL;
        if (root->type == WT_PAGE_ROW_INT) {
            __wt_ref_key(root, *root_refp, &p, &size);
            WT_ERR(__wt_row_ikey(session, 0, p, size, ref));
            root_incr += sizeof(WT_IKEY) + size;
        } else
            ref->ref_recno = (*root_refp)->ref_recno;
        F_SET(ref, WT_REF_FLAG_INTERNAL);
        WT_REF_SET_STATE(ref, WT_REF_MEM);

        /*
         * Initialize the child page. Block eviction in newly created pages and mark them dirty.
         */
        child->pg_intl_parent_ref = ref;
        WT_ERR(__wt_page_modify_init(session, child));
        __wt_page_modify_set(session, child);

        /*
         * The newly allocated child's page index references the same structures as the root. (We
         * cannot move WT_REF structures, threads may be underneath us right now changing the
         * structure state.) However, if the WT_REF structures reference on-page information, we
         * have to fix that, because the disk image for the page that has a page index entry for the
         * WT_REF is about to change.
         */
        child_pindex = WT_INTL_INDEX_GET_SAFE(child);
        child_incr = 0;
        for (child_refp = child_pindex->index, j = 0; j < slots; ++child_refp, ++root_refp, ++j)
            WT_ERR(__split_ref_move(session, root, root_refp, &root_decr, child_refp, &child_incr));

        __wt_cache_page_inmem_incr(session, child, child_incr);
    }
    WT_ASSERT(session, alloc_refp - alloc_index->index == (ptrdiff_t)alloc_index->entries);
    WT_ASSERT(session, root_refp - pindex->index == (ptrdiff_t)pindex->entries);

    /*
     * Flush our writes and start making real changes to the tree, errors are fatal.
     */
    WT_PUBLISH(complete, WT_ERR_PANIC);

    /* Prepare the WT_REFs for the move. */
    WT_ERR(__split_ref_prepare(session, alloc_index, &locked, false));

    /* Encourage a race */
    __wt_timing_stress(session, WT_TIMING_STRESS_SPLIT_1);

    /*
     * Confirm the root page's index hasn't moved, then update it, which makes the split visible to
     * threads descending the tree.
     */
    WT_ASSERT(session, WT_INTL_INDEX_GET_SAFE(root) == pindex);
    WT_INTL_INDEX_SET(root, alloc_index);
    alloc_index = NULL;

    /* Encourage a race */
    __wt_timing_stress(session, WT_TIMING_STRESS_SPLIT_2);

    /*
     * Mark the root page with the split generation.
     *
     * Note: as the root page cannot currently be evicted, the root split generation isn't ever
     * used. That said, it future proofs eviction and isn't expensive enough to special-case.
     */
    WT_FULL_BARRIER();
    split_gen = __wt_gen(session, WT_GEN_SPLIT);
    root->pg_intl_split_gen = split_gen;

    /* Finalize the WT_REF move. */
    __split_ref_final(session, split_gen, &locked);

#ifdef HAVE_DIAGNOSTIC
    WT_WITH_PAGE_INDEX(session, ret = __split_verify_root(session, root));
    WT_ERR(ret);
#endif

    /* The split is complete and verified, ignore benign errors. */
    complete = WT_ERR_IGNORE;

    /*
     * We can't free the previous root's index, there may be threads using it. Add to the session's
     * discard list, to be freed once we know no threads can still be using it.
     *
     * This change requires care with error handling: we have already updated the page with a new
     * index. Even if stashing the old value fails, we don't roll back that change, because threads
     * may already be using the new index.
     */
    size = sizeof(WT_PAGE_INDEX) + pindex->entries * sizeof(WT_REF *);
    WT_TRET(__split_safe_free(session, split_gen, false, pindex, size));
    root_decr += size;

    /* Adjust the root's memory footprint. */
    __wt_cache_page_inmem_incr(session, root, root_incr);
    __wt_cache_page_inmem_decr(session, root, root_decr);

    __wt_gen_next(session, WT_GEN_SPLIT, NULL);
err:
    __split_ref_final(session, 0, &locked);

    switch (complete) {
    case WT_ERR_RETURN:
        __wt_free_ref_index(session, root, alloc_index, true);
        break;
    case WT_ERR_IGNORE:
        if (ret != WT_PANIC) {
            if (ret != 0)
                __wt_err(session, ret,
                  "ignoring not-fatal error during root page split to deepen the tree");
            ret = 0;
            break;
        }
    /* FALLTHROUGH */
    case WT_ERR_PANIC:
        ret = __wt_panic(session, ret, "fatal error during root page split to deepen the tree");
        break;
    }
    return (ret);
}

/*
 * __split_parent_discard_ref --
 *     Worker routine to discard WT_REFs for the split-parent function.
 */
static int
__split_parent_discard_ref(WT_SESSION_IMPL *session, WT_REF *ref, WT_PAGE *parent, size_t *decrp,
  uint64_t split_gen, bool exclusive)
{
    WT_DECL_RET;
    WT_IKEY *ikey;
    size_t size;

    /*
     * Row-store trees where the old version of the page is being discarded: the previous parent
     * page's key for this child page may have been an on-page overflow key. In that case, if the
     * key hasn't been deleted, delete it now, including its backing blocks. We are exchanging the
     * WT_REF that referenced it for the split page WT_REFs and their keys, and there's no longer
     * any reference to it. Done after completing the split (if we failed, we'd leak the underlying
     * blocks, but the parent page would be unaffected).
     */
    if (parent->type == WT_PAGE_ROW_INT) {
        WT_TRET(__split_ovfl_key_cleanup(session, parent, ref));
        ikey = __wt_ref_key_instantiated(ref);
        if (ikey != NULL) {
            size = sizeof(WT_IKEY) + ikey->size;
            WT_TRET(__split_safe_free(session, split_gen, exclusive, ikey, size));
            *decrp += size;
        }
    }

    /* Free any backing fast-truncate memory. */
    __wt_free(session, ref->ft_info.del);

    /* Free the backing block and address. */
    WT_TRET(__wt_ref_block_free(session, ref));

    /*
     * Set the WT_REF state. It may be possible to immediately free the WT_REF, so this is our last
     * chance.
     */
    WT_REF_SET_STATE(ref, WT_REF_SPLIT);

    WT_TRET(__split_safe_free(session, split_gen, exclusive, ref, sizeof(WT_REF)));
    *decrp += sizeof(WT_REF);

    return (ret);
}

/*
 * __split_parent --
 *     Resolve a multi-page split, inserting new information into the parent.
 */
static int
__split_parent(WT_SESSION_IMPL *session, WT_REF *ref, WT_REF **ref_new, uint32_t new_entries,
  size_t parent_incr, bool exclusive, bool discard)
{
    WT_BTREE *btree;
    WT_DECL_ITEM(scr);
    WT_DECL_RET;
    WT_PAGE *parent;
    WT_PAGE_INDEX *alloc_index, *pindex;
    WT_REF **alloc_refp, *next_ref;
    WT_SPLIT_ERROR_PHASE complete;
    size_t parent_decr, size;
    uint64_t split_gen;
    uint32_t deleted_entries, parent_entries, result_entries;
    uint32_t *deleted_refs;
    uint32_t hint, i, j;
    bool empty_parent;

    btree = S2BT(session);
    parent = ref->home;

    alloc_index = pindex = NULL;
    parent_decr = 0;
    deleted_refs = NULL;
    empty_parent = false;
    complete = WT_ERR_RETURN;

    /* Mark the page dirty. */
    WT_RET(__wt_page_modify_init(session, parent));
    __wt_page_modify_set(session, parent);

    /*
     * We've locked the parent, which means it cannot split (which is the only reason to worry about
     * split generation values).
     */
    pindex = WT_INTL_INDEX_GET_SAFE(parent);
    parent_entries = pindex->entries;

    /*
     * Remove any refs to deleted pages while we are splitting, we have the internal page locked
     * down and are copying the refs into a new page-index array anyway.
     *
     * We can't do this if there is a sync running in the tree in another session: removing the refs
     * frees the blocks for the deleted pages, which can corrupt the free list calculated by the
     * sync.
     */
    deleted_entries = 0;
    if (!WT_BTREE_SYNCING(btree) || WT_SESSION_BTREE_SYNC(session))
        for (i = 0; i < parent_entries; ++i) {
            next_ref = pindex->index[i];
            WT_ASSERT(session, next_ref->state != WT_REF_SPLIT);

            /* Protect against including the replaced WT_REF in the list of deleted items. */
            if (next_ref != ref && next_ref->state == WT_REF_DELETED &&
              __wt_delete_page_skip(session, next_ref, true) &&
              WT_REF_CAS_STATE(session, next_ref, WT_REF_DELETED, WT_REF_LOCKED)) {
                if (scr == NULL)
                    WT_ERR(__wt_scr_alloc(session, 10 * sizeof(uint32_t), &scr));
                WT_ERR(__wt_buf_grow(session, scr, (deleted_entries + 1) * sizeof(uint32_t)));
                deleted_refs = scr->mem;
                deleted_refs[deleted_entries++] = i;
            }
        }

    /*
     * The final entry count is the original count, where one entry will be replaced by some number
     * of new entries, and some number will be deleted.
     */
    result_entries = (parent_entries + (new_entries - 1)) - deleted_entries;

    /*
     * If there are no remaining entries on the parent, give up, we can't leave an empty internal
     * page. Mark it to be evicted soon and clean up any references that have changed state.
     */
    if (result_entries == 0) {
        empty_parent = true;
        if (!__wt_ref_is_root(parent->pg_intl_parent_ref))
            __wt_page_evict_soon(session, parent->pg_intl_parent_ref);
        goto err;
    }

    /*
     * Allocate and initialize a new page index array for the parent, then copy references from the
     * original index array, plus references from the newly created split array, into place.
     *
     * Update the WT_REF's page-index hint as we go. This can race with a thread setting the hint
     * based on an older page-index, and the change isn't backed out in the case of an error, so
     * there ways for the hint to be wrong; OK because it's just a hint.
     */
    size = sizeof(WT_PAGE_INDEX) + result_entries * sizeof(WT_REF *);
    WT_ERR(__wt_calloc(session, 1, size, &alloc_index));
    parent_incr += size;
    alloc_index->index = (WT_REF **)(alloc_index + 1);
    alloc_index->entries = result_entries;
    for (alloc_refp = alloc_index->index, hint = i = 0; i < parent_entries; ++i) {
        next_ref = pindex->index[i];
        if (next_ref == ref) {
            for (j = 0; j < new_entries; ++j) {
                ref_new[j]->home = parent;
                ref_new[j]->pindex_hint = hint++;
                *alloc_refp++ = ref_new[j];
            }
            continue;
        }

        /* Skip refs we have marked for deletion. */
        if (deleted_entries != 0) {
            for (j = 0; j < deleted_entries; ++j)
                if (deleted_refs[j] == i)
                    break;
            if (j < deleted_entries)
                continue;
        }

        next_ref->pindex_hint = hint++;
        *alloc_refp++ = next_ref;
    }

    /* Check we filled in the expected number of entries. */
    WT_ASSERT(session, alloc_refp - alloc_index->index == (ptrdiff_t)result_entries);

    /* Start making real changes to the tree, errors are fatal. */
    WT_NOT_READ(complete, WT_ERR_PANIC);

    /* Encourage a race */
    __wt_timing_stress(session, WT_TIMING_STRESS_SPLIT_3);

    /*
     * Confirm the parent page's index hasn't moved then update it, which makes the split visible to
     * threads descending the tree.
     */
    WT_ASSERT(session, WT_INTL_INDEX_GET_SAFE(parent) == pindex);
    WT_INTL_INDEX_SET(parent, alloc_index);
    alloc_index = NULL;

    /* Encourage a race */
    __wt_timing_stress(session, WT_TIMING_STRESS_SPLIT_4);

    /*
     * Get a generation for this split, mark the page. This must be after the new index is swapped
     * into place in order to know that no readers with the new generation will look at the old
     * index.
     */
    WT_FULL_BARRIER();
    split_gen = __wt_gen(session, WT_GEN_SPLIT);
    parent->pg_intl_split_gen = split_gen;

#ifdef HAVE_DIAGNOSTIC
    WT_WITH_PAGE_INDEX(session, __split_verify_intl_key_order(session, parent));
#endif

    /* The split is complete and verified, ignore benign errors. */
    complete = WT_ERR_IGNORE;

    /*
     * The new page index is in place. Threads cursoring in the tree are blocked because the WT_REF
     * being discarded (if any), and deleted WT_REFs (if any) are in a locked state. Changing the
     * locked state to split unblocks those threads and causes them to re-calculate their position
     * based on the just-updated parent page's index. The split state doesn't lock the WT_REF.addr
     * information which is read by cursor threads in some tree-walk cases: free the WT_REF we were
     * splitting and any deleted WT_REFs we found, modulo the usual safe free semantics, then reset
     * the WT_REF state.
     */
    if (discard) {
        WT_ASSERT(session, exclusive || ref->state == WT_REF_LOCKED);
        WT_TRET(
          __split_parent_discard_ref(session, ref, parent, &parent_decr, split_gen, exclusive));
    }
    for (i = 0; i < deleted_entries; ++i) {
        next_ref = pindex->index[deleted_refs[i]];
        WT_ASSERT(session, next_ref->state == WT_REF_LOCKED);
        WT_TRET(__split_parent_discard_ref(
          session, next_ref, parent, &parent_decr, split_gen, exclusive));
    }

    /*
     * !!!
     * The original WT_REF has now been freed, we can no longer look at it.
     */

    /*
     * Don't cache the change: not required for correctness, but stops threads spinning on incorrect
     * page references.
     */
    WT_FULL_BARRIER();

    /*
     * We can't free the previous page index, there may be threads using it. Add it to the session
     * discard list, to be freed when it's safe.
     */
    size = sizeof(WT_PAGE_INDEX) + pindex->entries * sizeof(WT_REF *);
    WT_TRET(__split_safe_free(session, split_gen, exclusive, pindex, size));
    parent_decr += size;

    /* Adjust the parent's memory footprint. */
    __wt_cache_page_inmem_incr(session, parent, parent_incr);
    __wt_cache_page_inmem_decr(session, parent, parent_decr);

    /*
     * We've discarded the WT_REFs and swapping in a new page index released the page for eviction;
     * we can no longer look inside the WT_REF or the page, be careful logging the results.
     */
    __wt_verbose(session, WT_VERB_SPLIT,
      "%p: split into parent, %" PRIu32 "->%" PRIu32 ", %" PRIu32 " deleted", (void *)ref,
      parent_entries, result_entries, deleted_entries);

    __wt_gen_next(session, WT_GEN_SPLIT, NULL);
err:
    /*
     * A note on error handling: if we completed the split, return success, nothing really bad can
     * have happened, and our caller has to proceed with the split.
     */
    switch (complete) {
    case WT_ERR_RETURN:
        /* Unlock WT_REFs locked because they were in a deleted state. */
        for (i = 0; i < deleted_entries; ++i) {
            next_ref = pindex->index[deleted_refs[i]];
            WT_ASSERT(session, next_ref->state == WT_REF_LOCKED);
            WT_REF_SET_STATE(next_ref, WT_REF_DELETED);
        }

        __wt_free_ref_index(session, NULL, alloc_index, false);
        /*
         * The split couldn't proceed because the parent would be empty, return EBUSY so our caller
         * knows to unlock the WT_REF that's being deleted, but don't be noisy, there's nothing
         * wrong.
         */
        if (empty_parent)
            ret = __wt_set_return(session, EBUSY);
        break;
    case WT_ERR_IGNORE:
        if (ret != WT_PANIC) {
            if (ret != 0)
                __wt_err(session, ret, "ignoring not-fatal error during parent page split");
            ret = 0;
            break;
        }
    /* FALLTHROUGH */
    case WT_ERR_PANIC:
        ret = __wt_panic(session, ret, "fatal error during parent page split");
        break;
    }
    __wt_scr_free(session, &scr);
    return (ret);
}

/*
 * __split_internal --
 *     Split an internal page into its parent.
 */
static int
__split_internal(WT_SESSION_IMPL *session, WT_PAGE *parent, WT_PAGE *page)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_PAGE *child, **locked;
    WT_PAGE_INDEX *alloc_index, *child_pindex, *pindex, *replace_index;
    WT_REF **alloc_refp, **child_refp, *page_ref, **page_refp, *ref;
    WT_SPLIT_ERROR_PHASE complete;
    size_t child_incr, page_decr, page_incr, parent_incr, size;
    uint64_t split_gen;
    uint32_t children, chunk, i, j, remain;
    uint32_t slots;
    void *p;

    WT_STAT_CONN_DATA_INCR(session, cache_eviction_split_internal);

    /* Mark the page dirty. */
    WT_RET(__wt_page_modify_init(session, page));
    __wt_page_modify_set(session, page);

    btree = S2BT(session);
    alloc_index = replace_index = NULL;
    page_ref = page->pg_intl_parent_ref;
    locked = NULL;
    page_decr = page_incr = parent_incr = 0;
    complete = WT_ERR_RETURN;

    /*
     * Our caller is holding the page locked to single-thread splits, which means we can safely look
     * at the page's index without setting a split generation.
     */
    pindex = WT_INTL_INDEX_GET_SAFE(page);

    /*
     * Decide how many child pages to create, then calculate the standard chunk and whatever
     * remains. Sanity check the number of children: the decision to split matched to the
     * deepen-per-child configuration might get it wrong.
     */
    children = pindex->entries / btree->split_deepen_per_child;
    if (children < 10) {
        if (pindex->entries < 100)
            return (__wt_set_return(session, EBUSY));
        children = 10;
    }
    chunk = pindex->entries / children;
    remain = pindex->entries - chunk * (children - 1);

    __wt_verbose(session, WT_VERB_SPLIT,
      "%p: %" PRIu32 " internal page elements, splitting %" PRIu32 " children into parent %p",
      (void *)page, pindex->entries, children, (void *)parent);

    /*
     * Ideally, we'd discard the original page, but that's hard since other threads of control are
     * using it (for example, if eviction is walking the tree and looking at the page.) Instead,
     * perform a right-split, moving all except the first chunk of the page's WT_REF objects to new
     * pages.
     *
     * Create and initialize a replacement WT_PAGE_INDEX for the original page.
     */
    size = sizeof(WT_PAGE_INDEX) + chunk * sizeof(WT_REF *);
    WT_ERR(__wt_calloc(session, 1, size, &replace_index));
    page_incr += size;
    replace_index->index = (WT_REF **)(replace_index + 1);
    replace_index->entries = chunk;
    for (page_refp = pindex->index, i = 0; i < chunk; ++i)
        replace_index->index[i] = *page_refp++;

    /*
     * Allocate a new WT_PAGE_INDEX and set of WT_REF objects to be inserted into the page's parent,
     * replacing the page's page-index.
     *
     * The first slot of the new WT_PAGE_INDEX is the original page WT_REF. The remainder of the
     * slots are allocated WT_REFs.
     */
    size = sizeof(WT_PAGE_INDEX) + children * sizeof(WT_REF *);
    WT_ERR(__wt_calloc(session, 1, size, &alloc_index));
    parent_incr += size;
    alloc_index->index = (WT_REF **)(alloc_index + 1);
    alloc_index->entries = children;
    alloc_refp = alloc_index->index;
    *alloc_refp++ = page_ref;
    for (i = 1; i < children; ++alloc_refp, ++i)
        WT_ERR(__wt_calloc_one(session, alloc_refp));
    parent_incr += children * sizeof(WT_REF);

    /* Allocate child pages, and connect them into the new page index. */
    WT_ASSERT(session, page_refp == pindex->index + chunk);
    for (alloc_refp = alloc_index->index + 1, i = 1; i < children; ++i) {
        slots = i == children - 1 ? remain : chunk;

        WT_ERR(__wt_page_alloc(session, page->type, slots, false, &child));

        /*
         * Initialize the page's child reference; we need a copy of the page's key.
         */
        ref = *alloc_refp++;
        ref->home = parent;
        ref->page = child;
        ref->addr = NULL;
        if (page->type == WT_PAGE_ROW_INT) {
            __wt_ref_key(page, *page_refp, &p, &size);
            WT_ERR(__wt_row_ikey(session, 0, p, size, ref));
            parent_incr += sizeof(WT_IKEY) + size;
        } else
            ref->ref_recno = (*page_refp)->ref_recno;
        F_SET(ref, WT_REF_FLAG_INTERNAL);
        WT_REF_SET_STATE(ref, WT_REF_MEM);

        /*
         * Initialize the child page. Block eviction in newly created pages and mark them dirty.
         */
        child->pg_intl_parent_ref = ref;
        WT_ERR(__wt_page_modify_init(session, child));
        __wt_page_modify_set(session, child);

        /*
         * The newly allocated child's page index references the same structures as the parent. (We
         * cannot move WT_REF structures, threads may be underneath us right now changing the
         * structure state.) However, if the WT_REF structures reference on-page information, we
         * have to fix that, because the disk image for the page that has a page index entry for the
         * WT_REF is about to be discarded.
         */
        child_pindex = WT_INTL_INDEX_GET_SAFE(child);
        child_incr = 0;
        for (child_refp = child_pindex->index, j = 0; j < slots; ++child_refp, ++page_refp, ++j)
            WT_ERR(__split_ref_move(session, page, page_refp, &page_decr, child_refp, &child_incr));

        __wt_cache_page_inmem_incr(session, child, child_incr);
    }
    WT_ASSERT(session, alloc_refp - alloc_index->index == (ptrdiff_t)alloc_index->entries);
    WT_ASSERT(session, page_refp - pindex->index == (ptrdiff_t)pindex->entries);

    /*
     * Flush our writes and start making real changes to the tree, errors are fatal.
     */
    WT_PUBLISH(complete, WT_ERR_PANIC);

    /* Prepare the WT_REFs for the move. */
    WT_ERR(__split_ref_prepare(session, alloc_index, &locked, true));

    /* Encourage a race */
    __wt_timing_stress(session, WT_TIMING_STRESS_SPLIT_5);

    /* Split into the parent. */
    WT_ERR(__split_parent(
      session, page_ref, alloc_index->index, alloc_index->entries, parent_incr, false, false));

    /*
     * Confirm the page's index hasn't moved, then update it, which makes the split visible to
     * threads descending the tree.
     */
    WT_ASSERT(session, WT_INTL_INDEX_GET_SAFE(page) == pindex);
    WT_INTL_INDEX_SET(page, replace_index);

    /* Encourage a race */
    __wt_timing_stress(session, WT_TIMING_STRESS_SPLIT_6);

    /*
     * Get a generation for this split, mark the parent page. This must be after the new index is
     * swapped into place in order to know that no readers with the new generation will look at the
     * old index.
     */
    WT_FULL_BARRIER();
    split_gen = __wt_gen(session, WT_GEN_SPLIT);
    page->pg_intl_split_gen = split_gen;

    /* Finalize the WT_REF move. */
    __split_ref_final(session, split_gen, &locked);

#ifdef HAVE_DIAGNOSTIC
    WT_WITH_PAGE_INDEX(session, __split_verify_intl_key_order(session, parent));
    WT_WITH_PAGE_INDEX(session, __split_verify_intl_key_order(session, page));
#endif

    /* The split is complete and verified, ignore benign errors. */
    complete = WT_ERR_IGNORE;

    /*
     * We don't care about the page-index we allocated, all we needed was the array of WT_REF
     * structures, which has now been split into the parent page.
     */
    __wt_free(session, alloc_index);

    /*
     * We can't free the previous page's index, there may be threads using it. Add to the session's
     * discard list, to be freed once we know no threads can still be using it.
     *
     * This change requires care with error handling, we've already updated the parent page. Even if
     * stashing the old value fails, we don't roll back that change, because threads may already be
     * using the new parent page.
     */
    size = sizeof(WT_PAGE_INDEX) + pindex->entries * sizeof(WT_REF *);
    WT_TRET(__split_safe_free(session, split_gen, false, pindex, size));
    page_decr += size;

    /* Adjust the page's memory footprint. */
    __wt_cache_page_inmem_incr(session, page, page_incr);
    __wt_cache_page_inmem_decr(session, page, page_decr);

    __wt_gen_next(session, WT_GEN_SPLIT, NULL);
err:
    __split_ref_final(session, 0, &locked);

    switch (complete) {
    case WT_ERR_RETURN:
        /*
         * The replace-index variable is the internal page being split's new page index, referencing
         * the first chunk of WT_REFs that aren't being moved to other pages. Those WT_REFs survive
         * the failure, they're referenced from the page's current index. Simply free that memory,
         * but nothing it references.
         */
        __wt_free(session, replace_index);

        /*
         * The alloc-index variable is the array of new WT_REF entries intended to be inserted into
         * the page being split's parent.
         *
         * Except for the first slot (the original page's WT_REF), it's an array of newly allocated
         * combined WT_PAGE_INDEX and WT_REF structures, each of which references a newly allocated
         * (and modified) child page, each of which references an index of WT_REFs from the page
         * being split. Free everything except for slot 1 and the WT_REFs in the child page indexes.
         *
         * First, skip slot 1. Second, we want to free all of the child pages referenced from the
         * alloc-index array, but we can't just call the usual discard function because the WT_REFs
         * referenced by the child pages remain referenced by the original page, after error. For
         * each entry, free the child page's page index (so the underlying page-free function will
         * ignore it), then call the general-purpose discard function.
         */
        if (alloc_index == NULL)
            break;
        alloc_refp = alloc_index->index;
        *alloc_refp++ = NULL;
        for (i = 1; i < children; ++alloc_refp, ++i) {
            ref = *alloc_refp;
            if (ref == NULL || ref->page == NULL)
                continue;

            child = ref->page;
            child_pindex = WT_INTL_INDEX_GET_SAFE(child);
            __wt_free(session, child_pindex);
            WT_INTL_INDEX_SET(child, NULL);
        }
        __wt_free_ref_index(session, page, alloc_index, true);
        break;
    case WT_ERR_IGNORE:
        if (ret != WT_PANIC) {
            if (ret != 0)
                __wt_err(session, ret, "ignoring not-fatal error during internal page split");
            ret = 0;
            break;
        }
    /* FALLTHROUGH */
    case WT_ERR_PANIC:
        ret = __wt_panic(session, ret, "fatal error during internal page split");
        break;
    }
    return (ret);
}

/*
 * __split_internal_lock --
 *     Lock an internal page.
 */
static int
__split_internal_lock(WT_SESSION_IMPL *session, WT_REF *ref, bool trylock, WT_PAGE **parentp)
{
    WT_PAGE *parent;

    *parentp = NULL;

    /*
     * A checkpoint reconciling this parent page can deadlock with our split. We have an exclusive
     * page lock on the child before we acquire the page's reconciliation lock, and reconciliation
     * acquires the page's reconciliation lock before it encounters the child's exclusive lock
     * (which causes reconciliation to loop until the exclusive lock is resolved). If we want to
     * split the parent, give up to avoid that deadlock.
     */
    if (!trylock && !__wt_btree_can_evict_dirty(session))
        return (__wt_set_return(session, EBUSY));

    /*
     * Get a page-level lock on the parent to single-thread splits into the page because we need to
     * single-thread sizing/growing the page index. It's OK to queue up multiple splits as the child
     * pages split, but the actual split into the parent has to be serialized. Note we allocate
     * memory inside of the lock and may want to invest effort in making the locked period shorter.
     *
     * We use the reconciliation lock here because not only do we have to single-thread the split,
     * we have to lock out reconciliation of the parent because reconciliation of the parent can't
     * deal with finding a split child during internal page traversal. Basically, there's no reason
     * to use a different lock if we have to block reconciliation anyway.
     */
    for (;;) {
        parent = ref->home;

        /* Encourage races. */
        __wt_timing_stress(session, WT_TIMING_STRESS_SPLIT_7);

        /* Page locks live in the modify structure. */
        WT_RET(__wt_page_modify_init(session, parent));

        if (trylock)
            WT_RET(WT_PAGE_TRYLOCK(session, parent));
        else
            WT_PAGE_LOCK(session, parent);
        if (parent == ref->home)
            break;
        WT_PAGE_UNLOCK(session, parent);
    }

    /*
     * This child has exclusive access to split its parent and the child's existence prevents the
     * parent from being evicted. However, once we update the parent's index, it may no longer refer
     * to the child, and could conceivably be evicted. If the parent page is dirty, our page lock
     * prevents eviction because reconciliation is blocked. However, if the page were clean, it
     * could be evicted without encountering our page lock. That isn't possible because you cannot
     * move a child page and still leave the parent page clean.
     */

    *parentp = parent;
    return (0);
}

/*
 * __split_internal_unlock --
 *     Unlock the parent page.
 */
static void
__split_internal_unlock(WT_SESSION_IMPL *session, WT_PAGE *parent)
{
    WT_PAGE_UNLOCK(session, parent);
}

/*
 * __split_internal_should_split --
 *     Return if we should split an internal page.
 */
static bool
__split_internal_should_split(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_BTREE *btree;
    WT_PAGE *page;
    WT_PAGE_INDEX *pindex;

    btree = S2BT(session);
    page = ref->page;

    /*
     * Our caller is holding the parent page locked to single-thread splits, which means we can
     * safely look at the page's index without setting a split generation.
     */
    pindex = WT_INTL_INDEX_GET_SAFE(page);

    /* Sanity check for a reasonable number of on-page keys. */
    if (pindex->entries < 100)
        return (false);

    /*
     * Deepen the tree if the page's memory footprint is larger than the maximum size for a page in
     * memory (presumably putting eviction pressure on the cache).
     */
    if (page->memory_footprint > btree->maxmempage)
        return (true);

    /*
     * Check if the page has enough keys to make it worth splitting. If the number of keys is
     * allowed to grow too large, the cost of splitting into parent pages can become large enough to
     * result in slow operations.
     */
    if (pindex->entries > btree->split_deepen_min_child)
        return (true);

    return (false);
}

/*
 * __split_parent_climb --
 *     Check if we should split up the tree.
 */
static int
__split_parent_climb(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_DECL_RET;
    WT_PAGE *parent;
    WT_REF *ref;

    /*
     * Disallow internal splits during the final pass of a checkpoint. Most splits are already
     * disallowed during checkpoints, but an important exception is insert splits. The danger is an
     * insert split creates a new chunk of the namespace, and then the internal split will move it
     * to a different part of the tree where it will be written; in other words, in one part of the
     * tree we'll skip the newly created insert split chunk, but we'll write it upon finding it in a
     * different part of the tree.
     */
    if (!__wt_btree_can_evict_dirty(session)) {
        __split_internal_unlock(session, page);
        return (0);
    }

    /*
     * Page splits trickle up the tree, that is, as leaf pages grow large enough and are evicted,
     * they'll split into their parent. And, as that parent page grows large enough and is evicted,
     * it splits into its parent and so on. When the page split wave reaches the root, the tree will
     * permanently deepen as multiple root pages are written.
     *
     * However, this only helps if internal pages are evicted (and we resist evicting internal pages
     * for obvious reasons), or if the tree were to be closed and re-opened from a disk image, which
     * may be a rare event.
     *
     * To avoid internal pages becoming too large absent eviction, check parent pages each time
     * pages are split into them. If the page is big enough, either split the page into its parent
     * or, in the case of the root, deepen the tree.
     *
     * Split up the tree.
     */
    for (;;) {
        parent = NULL;
        ref = page->pg_intl_parent_ref;

        /* If we don't need to split the page, we're done. */
        if (!__split_internal_should_split(session, ref))
            break;

        /*
         * If we've reached the root page, there are no subsequent pages to review, deepen the tree
         * and quit.
         */
        if (__wt_ref_is_root(ref)) {
            ret = __split_root(session, page);
            break;
        }

        /*
         * Lock the parent and split into it, then swap the parent/page locks, lock-coupling up the
         * tree.
         */
        WT_ERR(__split_internal_lock(session, ref, true, &parent));
        ret = __split_internal(session, parent, page);
        __split_internal_unlock(session, page);

        page = parent;
        parent = NULL;
        WT_ERR(ret);
    }

err:
    if (parent != NULL)
        __split_internal_unlock(session, parent);
    __split_internal_unlock(session, page);

    /* A page may have been busy, in which case return without error. */
    switch (ret) {
    case 0:
    case WT_PANIC:
        break;
    case EBUSY:
        ret = 0;
        break;
    default:
        __wt_err(session, ret, "ignoring not-fatal error during parent page split");
        ret = 0;
        break;
    }
    return (ret);
}

/*
 * __split_multi_inmem --
 *     Instantiate a page from a disk image.
 */
static int
__split_multi_inmem(WT_SESSION_IMPL *session, WT_PAGE *orig, WT_MULTI *multi, WT_REF *ref)
{
    WT_CURSOR_BTREE cbt;
    WT_DECL_ITEM(key);
    WT_DECL_RET;
    WT_PAGE *page;
    WT_PAGE_MODIFY *mod;
    WT_SAVE_UPD *supd;
    WT_UPDATE *prev_onpage, *upd;
    uint64_t recno;
    uint32_t i, slot;
    bool prepare;

    /*
     * In 04/2016, we removed column-store record numbers from the WT_PAGE structure, leading to
     * hard-to-debug problems because we corrupt the page if we search it using the wrong initial
     * record number. For now, assert the record number is set.
     */
    WT_ASSERT(session, orig->type != WT_PAGE_COL_VAR || ref->ref_recno != 0);

    /*
     * This code re-creates an in-memory page from a disk image, and adds references to any
     * unresolved update chains to the new page. We get here either because an update could not be
     * written when evicting a page, or eviction chose to keep a page in memory.
     *
     * Reconciliation won't create a disk image with entries the running database no longer cares
     * about (at least, not based on the current tests we're performing), ignore the validity
     * window.
     *
     * Steal the disk image and link the page into the passed-in WT_REF to simplify error handling:
     * our caller will not discard the disk image when discarding the original page, and our caller
     * will discard the allocated page on error, when discarding the allocated WT_REF.
     */
    WT_RET(__wt_page_inmem(session, ref, multi->disk_image, WT_PAGE_DISK_ALLOC, &page, &prepare));
    multi->disk_image = NULL;

    /*
     * In-memory databases restore non-obsolete updates directly in this function, don't call the
     * underlying page functions to do it.
     */
    if (prepare && !F_ISSET(S2C(session), WT_CONN_IN_MEMORY))
        WT_RET(__wt_page_inmem_prepare(session, ref));

    /*
     * Put the re-instantiated page in the same LRU queue location as the original page, unless this
     * was a forced eviction, in which case we leave the new page with the read generation unset.
     * Eviction will set the read generation next time it visits this page.
     */
    if (!WT_READGEN_EVICT_SOON(orig->read_gen))
        page->read_gen = orig->read_gen;

    /*
     * If there are no updates to apply to the page, we're done. Otherwise, there are updates we
     * need to restore.
     */
    if (multi->supd_entries == 0)
        return (0);
    WT_ASSERT(session, multi->supd_restore);

    if (orig->type == WT_PAGE_ROW_LEAF)
        WT_RET(__wt_scr_alloc(session, 0, &key));

    __wt_btcur_init(session, &cbt);
    __wt_btcur_open(&cbt);

    /* Re-create each modification we couldn't write. */
    for (i = 0, supd = multi->supd; i < multi->supd_entries; ++i, ++supd) {
        /* Ignore update chains that don't need to be restored. */
        if (!supd->restore)
            continue;

        if (supd->ins == NULL) {
            /* Note: supd->ins is never null for column-store. */
            slot = WT_ROW_SLOT(orig, supd->rip);
            upd = orig->modify->mod_row_update[slot];
        } else
            upd = supd->ins->upd;

        /* We shouldn't restore an empty update chain. */
        WT_ASSERT(session, upd != NULL);

        /*
         * Truncate the onpage value and the older versions moved to the history store. We can't
         * truncate the updates for in memory database and fixed length column store as they don't
         * support the history sore. We can't free the truncated updates here as we may still fail.
         * If we fail, we will append them back to their original update chains. Truncate before we
         * restore them to ensure the size of the page is correct.
         */
        if (supd->onpage_upd != NULL && !F_ISSET(S2C(session), WT_CONN_IN_MEMORY) &&
          orig->type != WT_PAGE_COL_FIX) {
            /*
             * We have decided to restore this update chain so it must have newer updates than the
             * onpage value on it.
             */
            WT_ASSERT(session, upd != supd->onpage_upd);
            /*
             * Move the pointer to the position before the onpage value and truncate all the updates
             * starting from the onpage value.
             */
            for (prev_onpage = upd;
                 prev_onpage->next != NULL && prev_onpage->next != supd->onpage_upd;
                 prev_onpage = prev_onpage->next)
                ;
            WT_ASSERT(session, prev_onpage->next == supd->onpage_upd);
            prev_onpage->next = NULL;
        }

        switch (orig->type) {
        case WT_PAGE_COL_FIX:
        case WT_PAGE_COL_VAR:
            /* Build a key. */
            recno = WT_INSERT_RECNO(supd->ins);

            /* Search the page. */
            WT_ERR(__wt_col_search(&cbt, recno, ref, true, NULL));

            /* Apply the modification. */
#ifdef HAVE_DIAGNOSTIC
            WT_ERR(__wt_col_modify(&cbt, recno, NULL, upd, WT_UPDATE_INVALID, true, true));
#else
            WT_ERR(__wt_col_modify(&cbt, recno, NULL, upd, WT_UPDATE_INVALID, true));
#endif
            break;
        case WT_PAGE_ROW_LEAF:
            /* Build a key. */
            if (supd->ins == NULL)
                WT_ERR(__wt_row_leaf_key(session, orig, supd->rip, key, false));
            else {
                key->data = WT_INSERT_KEY(supd->ins);
                key->size = WT_INSERT_KEY_SIZE(supd->ins);
            }

            /* Search the page. */
            WT_ERR(__wt_row_search(&cbt, key, true, ref, true, NULL));

            /* Apply the modification. */
#ifdef HAVE_DIAGNOSTIC
            WT_ERR(__wt_row_modify(&cbt, key, NULL, upd, WT_UPDATE_INVALID, true, true));
#else
            WT_ERR(__wt_row_modify(&cbt, key, NULL, upd, WT_UPDATE_INVALID, true));
#endif
            break;
        default:
            WT_ERR(__wt_illegal_value(session, orig->type));
        }
    }

    /*
     * When modifying the page we set the first dirty transaction to the last transaction currently
     * running. However, the updates we made might be older than that. Set the first dirty
     * transaction to an impossibly old value so this page is never skipped in a checkpoint.
     */
    mod = page->modify;
    mod->first_dirty_txn = WT_TXN_FIRST;

    /*
     * Restore the previous page's modify state to avoid repeatedly attempting eviction on the same
     * page.
     */
    mod->last_evict_pass_gen = orig->modify->last_evict_pass_gen;
    mod->last_eviction_id = orig->modify->last_eviction_id;
    mod->last_eviction_timestamp = orig->modify->last_eviction_timestamp;
    mod->rec_max_txn = orig->modify->rec_max_txn;
    mod->rec_max_timestamp = orig->modify->rec_max_timestamp;

    /* Add the update/restore flag to any previous state. */
    mod->restore_state = orig->modify->restore_state;
    FLD_SET(mod->restore_state, WT_PAGE_RS_RESTORED);

err:
    /* Free any resources that may have been cached in the cursor. */
    WT_TRET(__wt_btcur_close(&cbt, true));

    __wt_scr_free(session, &key);
    return (ret);
}

/*
 * __split_multi_inmem_final --
 *     Discard moved update lists from the original page and free the updates written to the data
 *     store and the history store.
 */
static void
__split_multi_inmem_final(WT_SESSION_IMPL *session, WT_PAGE *orig, WT_MULTI *multi)
{
    WT_SAVE_UPD *supd;
    uint32_t i, slot;

    /* If we have saved updates, we must have decided to restore them to the new page. */
    WT_ASSERT(session, multi->supd_entries == 0 || multi->supd_restore);

    /*
     * We successfully created new in-memory pages. For error-handling reasons, we've left the
     * update chains referenced by both the original and new pages. We're ready to discard the
     * original page, terminate the original page's reference to any update list we moved and free
     * the updates written to the data store and the history store.
     */
    for (i = 0, supd = multi->supd; i < multi->supd_entries; ++i, ++supd) {
        /* We have finished restoration. Discard the update chains that aren't restored. */
        if (!supd->restore)
            continue;

        if (supd->ins == NULL) {
            /* Note: supd->ins is never null for column-store. */
            slot = WT_ROW_SLOT(orig, supd->rip);
            orig->modify->mod_row_update[slot] = NULL;
        } else
            supd->ins->upd = NULL;

        /* Free the updates written to the data store and the history store. */
        if (supd->onpage_upd != NULL && !F_ISSET(S2C(session), WT_CONN_IN_MEMORY) &&
          orig->type != WT_PAGE_COL_FIX)
            __wt_free_update_list(session, &supd->onpage_upd);
    }
}

/*
 * __split_multi_inmem_fail --
 *     Discard allocated pages after failure and append the onpage values back to the original
 *     update chains.
 */
static void
__split_multi_inmem_fail(WT_SESSION_IMPL *session, WT_PAGE *orig, WT_MULTI *multi, WT_REF *ref)
{
    WT_SAVE_UPD *supd;
    WT_UPDATE *upd;
    uint32_t i, slot;

    if (!F_ISSET(S2C(session), WT_CONN_IN_MEMORY) && orig->type != WT_PAGE_COL_FIX)
        /* Append the onpage values back to the original update chains. */
        for (i = 0, supd = multi->supd; i < multi->supd_entries; ++i, ++supd) {
            /*
             * We don't need to do anything for update chains that are not restored, or restored
             * without an onpage value.
             */
            if (!supd->restore || supd->onpage_upd == NULL)
                continue;

            if (supd->ins == NULL) {
                /* Note: supd->ins is never null for column-store. */
                slot = WT_ROW_SLOT(orig, supd->rip);
                upd = orig->modify->mod_row_update[slot];
            } else
                upd = supd->ins->upd;

            WT_ASSERT(session, upd != NULL);

            for (; upd->next != NULL && upd->next != supd->onpage_upd; upd = upd->next)
                ;
            if (upd->next == NULL)
                upd->next = supd->onpage_upd;
        }

    /*
     * We failed creating new in-memory pages. For error-handling reasons, we've left the update
     * chains referenced by both the original and new pages. Discard the newly allocated WT_REF
     * structures and their pages (setting a flag so the discard code doesn't discard the updates on
     * the page).
     *
     * Our callers allocate WT_REF arrays, then individual WT_REFs, check for uninitialized
     * information.
     */
    if (ref != NULL) {
        if (ref->page != NULL)
            F_SET_ATOMIC(ref->page, WT_PAGE_UPDATE_IGNORE);
        __wt_free_ref(session, ref, orig->type, true);
    }
}

/*
 * __wt_multi_to_ref --
 *     Move a multi-block entry into a WT_REF structure.
 */
int
__wt_multi_to_ref(WT_SESSION_IMPL *session, WT_PAGE *page, WT_MULTI *multi, WT_REF **refp,
  size_t *incrp, bool closing)
{
    WT_ADDR *addr;
    WT_IKEY *ikey;
    WT_REF *ref;

    /* There can be an address or a disk image or both. */
    WT_ASSERT(session, multi->addr.addr != NULL || multi->disk_image != NULL);

    /* If closing the file, there better be an address. */
    WT_ASSERT(session, !closing || multi->addr.addr != NULL);

    /* If closing the file, there better not be any saved updates. */
    WT_ASSERT(session, !closing || multi->supd == NULL);

    /* If we don't have a disk image, we can't restore the saved updates. */
    WT_ASSERT(
      session, multi->disk_image != NULL || (multi->supd_entries == 0 && !multi->supd_restore));

    /* Verify any disk image we have. */
    WT_ASSERT(session,
      multi->disk_image == NULL ||
        __wt_verify_dsk_image(
          session, "[page instantiate]", multi->disk_image, 0, &multi->addr, true) == 0);

    /* Allocate an underlying WT_REF. */
    WT_RET(__wt_calloc_one(session, refp));
    ref = *refp;
    if (incrp)
        *incrp += sizeof(WT_REF);

    /*
     * Set the WT_REF key before (optionally) building the page, underlying column-store functions
     * need the page's key space to search it.
     */
    switch (page->type) {
    case WT_PAGE_ROW_INT:
    case WT_PAGE_ROW_LEAF:
        ikey = multi->key.ikey;
        WT_RET(__wt_row_ikey(session, 0, WT_IKEY_DATA(ikey), ikey->size, ref));
        if (incrp)
            *incrp += sizeof(WT_IKEY) + ikey->size;
        break;
    default:
        ref->ref_recno = multi->key.recno;
        break;
    }

    switch (page->type) {
    case WT_PAGE_COL_INT:
    case WT_PAGE_ROW_INT:
        F_SET(ref, WT_REF_FLAG_INTERNAL);
        break;
    default:
        F_SET(ref, WT_REF_FLAG_LEAF);
        break;
    }

    /*
     * If there's an address, the page was written, set it.
     *
     * Copy the address: we could simply take the buffer, but that would complicate error handling,
     * freeing the reference array would have to avoid freeing the memory, and it's not worth the
     * confusion.
     */
    if (multi->addr.addr != NULL) {
        WT_RET(__wt_calloc_one(session, &addr));
        ref->addr = addr;
        WT_TIME_AGGREGATE_COPY(&addr->ta, &multi->addr.ta);
        WT_RET(__wt_memdup(session, multi->addr.addr, multi->addr.size, &addr->addr));
        addr->size = multi->addr.size;
        addr->type = multi->addr.type;

        WT_REF_SET_STATE(ref, WT_REF_DISK);
    }

    /*
     * If we have a disk image and we're not closing the file, re-instantiate the page.
     *
     * Discard any page image we don't use.
     */
    if (multi->disk_image != NULL && !closing) {
        WT_RET(__split_multi_inmem(session, page, multi, ref));
        WT_REF_SET_STATE(ref, WT_REF_MEM);
    }
    __wt_free(session, multi->disk_image);

    return (0);
}

/*
 * __split_insert --
 *     Split a page's last insert list entries into a separate page.
 */
static int
__split_insert(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_DECL_RET;
    WT_INSERT *ins, **insp, *moved_ins, *prev_ins;
    WT_INSERT_HEAD *ins_head, *tmp_ins_head;
    WT_PAGE *page, *right;
    WT_REF *child, *split_ref[2] = {NULL, NULL};
    size_t key_size, page_decr, parent_incr, right_incr;
    uint8_t type;
    int i;
    void *key;

    WT_STAT_CONN_DATA_INCR(session, cache_inmem_split);

    page = ref->page;
    right = NULL;
    page_decr = parent_incr = right_incr = 0;
    type = page->type;

    /*
     * Assert splitting makes sense; specifically assert the page is dirty, we depend on that,
     * otherwise the page might be evicted based on its last reconciliation which no longer matches
     * reality after the split.
     */
    WT_ASSERT(session, __wt_leaf_page_can_split(session, page));
    WT_ASSERT(session, __wt_page_is_modified(page));
    WT_ASSERT(session, ref->ft_info.del == NULL);

    F_SET_ATOMIC(page, WT_PAGE_SPLIT_INSERT); /* Only split in-memory once. */

    /* Find the last item on the page. */
    if (type == WT_PAGE_ROW_LEAF)
        ins_head = page->entries == 0 ? WT_ROW_INSERT_SMALLEST(page) :
                                        WT_ROW_INSERT_SLOT(page, page->entries - 1);
    else
        ins_head = WT_COL_APPEND(page);
    moved_ins = WT_SKIP_LAST(ins_head);

    /*
     * The first page in the split is almost identical to the current page, but we have to create a
     * replacement WT_REF, the original WT_REF will be set to split status and eventually freed.
     */
    WT_ERR(__wt_calloc_one(session, &split_ref[0]));
    parent_incr += sizeof(WT_REF);
    child = split_ref[0];
    child->page = ref->page;
    child->home = ref->home;
    child->pindex_hint = ref->pindex_hint;
    F_SET(child, WT_REF_FLAG_LEAF);
    child->state = WT_REF_MEM; /* Visible as soon as the split completes. */
    child->addr = ref->addr;
    if (type == WT_PAGE_ROW_LEAF) {
        __wt_ref_key(ref->home, ref, &key, &key_size);
        WT_ERR(__wt_row_ikey(session, 0, key, key_size, child));
        parent_incr += sizeof(WT_IKEY) + key_size;
    } else
        child->ref_recno = ref->ref_recno;

    /*
     * The address has moved to the replacement WT_REF. Make sure it isn't freed when the original
     * ref is discarded.
     */
    ref->addr = NULL;

    /* The second page in the split is a new WT_REF/page pair. */
    WT_ERR(__wt_page_alloc(session, type, 0, false, &right));

    /*
     * The new page is dirty by definition, plus column-store splits update the page-modify
     * structure, so create it now.
     */
    WT_ERR(__wt_page_modify_init(session, right));
    __wt_page_modify_set(session, right);

    if (type == WT_PAGE_ROW_LEAF) {
        WT_ERR(__wt_calloc_one(session, &right->modify->mod_row_insert));
        WT_ERR(__wt_calloc_one(session, &right->modify->mod_row_insert[0]));
    } else {
        WT_ERR(__wt_calloc_one(session, &right->modify->mod_col_append));
        WT_ERR(__wt_calloc_one(session, &right->modify->mod_col_append[0]));
    }
    right_incr += sizeof(WT_INSERT_HEAD);
    right_incr += sizeof(WT_INSERT_HEAD *);

    WT_ERR(__wt_calloc_one(session, &split_ref[1]));
    parent_incr += sizeof(WT_REF);
    child = split_ref[1];
    child->page = right;
    F_SET(child, WT_REF_FLAG_LEAF);
    child->state = WT_REF_MEM; /* Visible as soon as the split completes. */
    if (type == WT_PAGE_ROW_LEAF) {
        WT_ERR(__wt_row_ikey(
          session, 0, WT_INSERT_KEY(moved_ins), WT_INSERT_KEY_SIZE(moved_ins), child));
        parent_incr += sizeof(WT_IKEY) + WT_INSERT_KEY_SIZE(moved_ins);
    } else
        child->ref_recno = WT_INSERT_RECNO(moved_ins);

    /*
     * Allocation operations completed, we're going to split.
     *
     * Record the split column-store page record, used in reconciliation.
     */
    if (type != WT_PAGE_ROW_LEAF) {
        WT_ASSERT(session, page->modify->mod_col_split_recno == WT_RECNO_OOB);
        page->modify->mod_col_split_recno = child->ref_recno;
    }

    /*
     * Calculate how much memory we're moving: figure out how deep the skip list stack is for the
     * element we are moving, and the memory used by the item's list of updates.
     */
    for (i = 0; i < WT_SKIP_MAXDEPTH && ins_head->tail[i] == moved_ins; ++i)
        ;
    WT_MEM_TRANSFER(page_decr, right_incr, sizeof(WT_INSERT) + (size_t)i * sizeof(WT_INSERT *));
    if (type == WT_PAGE_ROW_LEAF)
        WT_MEM_TRANSFER(page_decr, right_incr, WT_INSERT_KEY_SIZE(moved_ins));
    WT_MEM_TRANSFER(page_decr, right_incr, __wt_update_list_memsize(moved_ins->upd));

    /*
     * Move the last insert list item from the original page to the new page.
     *
     * First, update the item to the new child page. (Just append the entry for simplicity, the
     * previous skip list pointers originally allocated can be ignored.)
     */
    tmp_ins_head = type == WT_PAGE_ROW_LEAF ? right->modify->mod_row_insert[0] :
                                              right->modify->mod_col_append[0];
    tmp_ins_head->head[0] = tmp_ins_head->tail[0] = moved_ins;

    /*
     * Remove the entry from the orig page (i.e truncate the skip list).
     * Following is an example skip list that might help.
     *
     *               __
     *              |c3|
     *               |
     *   __		 __    __
     *  |a2|--------|c2|--|d2|
     *   |		 |	|
     *   __		 __    __	   __
     *  |a1|--------|c1|--|d1|--------|f1|
     *   |		 |	|	   |
     *   __    __    __    __    __    __
     *  |a0|--|b0|--|c0|--|d0|--|e0|--|f0|
     *
     *   From the above picture.
     *   The head array will be: a0, a1, a2, c3, NULL
     *   The tail array will be: f0, f1, d2, c3, NULL
     *   We are looking for: e1, d2, NULL
     *   If there were no f1, we'd be looking for: e0, NULL
     *   If there were an f2, we'd be looking for: e0, d1, d2, NULL
     *
     *   The algorithm does:
     *   1) Start at the top of the head list.
     *   2) Step down until we find a level that contains more than one
     *      element.
     *   3) Step across until we reach the tail of the level.
     *   4) If the tail is the item being moved, remove it.
     *   5) Drop down a level, and go to step 3 until at level 0.
     */
    prev_ins = NULL; /* -Wconditional-uninitialized */
    for (i = WT_SKIP_MAXDEPTH - 1, insp = &ins_head->head[i]; i >= 0; i--, insp--) {
        /* Level empty, or a single element. */
        if (ins_head->head[i] == NULL || ins_head->head[i] == ins_head->tail[i]) {
            /* Remove if it is the element being moved. */
            if (ins_head->head[i] == moved_ins)
                ins_head->head[i] = ins_head->tail[i] = NULL;
            continue;
        }

        for (ins = *insp; ins != ins_head->tail[i]; ins = ins->next[i])
            prev_ins = ins;

        /*
         * Update the stack head so that we step down as far to the right as possible. We know that
         * prev_ins is valid since levels must contain at least two items to be here.
         */
        insp = &prev_ins->next[i];
        if (ins == moved_ins) {
            /* Remove the item being moved. */
            WT_ASSERT(session, ins_head->head[i] != moved_ins);
            WT_ASSERT(session, prev_ins->next[i] == moved_ins);
            *insp = NULL;
            ins_head->tail[i] = prev_ins;
        }
    }

#ifdef HAVE_DIAGNOSTIC
    /*
     * Verify the moved insert item appears nowhere on the skip list.
     */
    for (i = WT_SKIP_MAXDEPTH - 1, insp = &ins_head->head[i]; i >= 0; i--, insp--)
        for (ins = *insp; ins != NULL; ins = ins->next[i])
            WT_ASSERT(session, ins != moved_ins);
#endif

    /*
     * We perform insert splits concurrently with checkpoints, where the requirement is a checkpoint
     * must include either the original page or both new pages. The page we're splitting is dirty,
     * but that's insufficient: set the first dirty transaction to an impossibly old value so this
     * page is not skipped by a checkpoint.
     */
    page->modify->first_dirty_txn = WT_TXN_FIRST;

    /*
     * We modified the page above, which will have set the first dirty transaction to the last
     * transaction current running. However, the updates we installed may be older than that. Set
     * the first dirty transaction to an impossibly old value so this page is never skipped in a
     * checkpoint.
     */
    right->modify->first_dirty_txn = WT_TXN_FIRST;

    /*
     * Update the page accounting.
     */
    __wt_cache_page_inmem_decr(session, page, page_decr);
    __wt_cache_page_inmem_incr(session, right, right_incr);

    /*
     * The act of splitting into the parent releases the pages for eviction; ensure the page
     * contents are consistent.
     */
    WT_WRITE_BARRIER();

    /*
     * Split into the parent.
     */
    if ((ret = __split_parent(session, ref, split_ref, 2, parent_incr, false, true)) == 0)
        return (0);

    /*
     * Failure.
     *
     * Reset the split column-store page record.
     */
    if (type != WT_PAGE_ROW_LEAF)
        page->modify->mod_col_split_recno = WT_RECNO_OOB;

    /*
     * Clear the allocated page's reference to the moved insert list element so it's not freed when
     * we discard the page.
     *
     * Move the element back to the original page list. For simplicity, the previous skip list
     * pointers originally allocated can be ignored, just append the entry to the end of the level 0
     * list. As before, we depend on the list having multiple elements and ignore the edge cases
     * small lists have.
     */
    if (type == WT_PAGE_ROW_LEAF)
        right->modify->mod_row_insert[0]->head[0] = right->modify->mod_row_insert[0]->tail[0] =
          NULL;
    else
        right->modify->mod_col_append[0]->head[0] = right->modify->mod_col_append[0]->tail[0] =
          NULL;

    ins_head->tail[0]->next[0] = moved_ins;
    ins_head->tail[0] = moved_ins;

    /* Fix up accounting for the page size. */
    __wt_cache_page_inmem_incr(session, page, page_decr);

err:
    if (split_ref[0] != NULL) {
        /*
         * The address was moved to the replacement WT_REF, restore it.
         */
        ref->addr = split_ref[0]->addr;

        if (type == WT_PAGE_ROW_LEAF)
            __wt_free(session, split_ref[0]->ref_ikey);
        __wt_free(session, split_ref[0]);
    }
    if (split_ref[1] != NULL) {
        if (type == WT_PAGE_ROW_LEAF)
            __wt_free(session, split_ref[1]->ref_ikey);
        __wt_free(session, split_ref[1]);
    }
    if (right != NULL) {
        /*
         * We marked the new page dirty; we're going to discard it, but first mark it clean and fix
         * up the cache statistics.
         */
        __wt_page_modify_clear(session, right);
        __wt_page_out(session, &right);
    }
    return (ret);
}

/*
 * __split_insert_lock --
 *     Split a page's last insert list entries into a separate page.
 */
static int
__split_insert_lock(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_DECL_RET;
    WT_PAGE *parent;

    /* Lock the parent page, then proceed with the insert split. */
    WT_RET(__split_internal_lock(session, ref, true, &parent));
    if ((ret = __split_insert(session, ref)) != 0) {
        __split_internal_unlock(session, parent);
        return (ret);
    }

    /*
     * Split up through the tree as necessary; we're holding the original parent page locked, note
     * the functions we call are responsible for releasing that lock.
     */
    return (__split_parent_climb(session, parent));
}

/*
 * __wt_split_insert --
 *     Split a page's last insert list entries into a separate page.
 */
int
__wt_split_insert(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_DECL_RET;

    __wt_verbose(session, WT_VERB_SPLIT, "%p: split-insert", (void *)ref);

    /*
     * Set the session split generation to ensure underlying code isn't surprised by internal page
     * eviction, then proceed with the insert split.
     */
    WT_WITH_PAGE_INDEX(session, ret = __split_insert_lock(session, ref));
    return (ret);
}

/*
 * __split_multi --
 *     Split a page into multiple pages.
 */
static int
__split_multi(WT_SESSION_IMPL *session, WT_REF *ref, bool closing)
{
    WT_DECL_RET;
    WT_PAGE *page;
    WT_PAGE_MODIFY *mod;
    WT_REF **ref_new;
    size_t parent_incr;
    uint32_t i, new_entries;

    WT_STAT_CONN_DATA_INCR(session, cache_eviction_split_leaf);

    page = ref->page;
    mod = page->modify;
    new_entries = mod->mod_multi_entries;

    parent_incr = 0;

    /*
     * Convert the split page's multiblock reconciliation information into an array of page
     * reference structures.
     */
    WT_RET(__wt_calloc_def(session, new_entries, &ref_new));
    for (i = 0; i < new_entries; ++i)
        WT_ERR(
          __wt_multi_to_ref(session, page, &mod->mod_multi[i], &ref_new[i], &parent_incr, closing));

    /*
     * Split into the parent; if we're closing the file, we hold it exclusively.
     */
    WT_ERR(__split_parent(session, ref, ref_new, new_entries, parent_incr, closing, true));

    /*
     * The split succeeded, we can no longer fail.
     *
     * Finalize the move, discarding moved update lists from the original page.
     */
    for (i = 0; i < new_entries; ++i)
        __split_multi_inmem_final(session, page, &mod->mod_multi[i]);

    /*
     * Pages with unresolved changes are not marked clean in reconciliation, do it now, then discard
     * the page.
     */
    __wt_page_modify_clear(session, page);
    __wt_page_out(session, &page);

    if (0) {
err:
        for (i = 0; i < new_entries; ++i)
            __split_multi_inmem_fail(session, page, &mod->mod_multi[i], ref_new[i]);
    }

    __wt_free(session, ref_new);
    return (ret);
}

/*
 * __split_multi_lock --
 *     Split a page into multiple pages.
 */
static int
__split_multi_lock(WT_SESSION_IMPL *session, WT_REF *ref, int closing)
{
    WT_DECL_RET;
    WT_PAGE *parent;

    /* Lock the parent page, then proceed with the split. */
    WT_RET(__split_internal_lock(session, ref, false, &parent));
    if ((ret = __split_multi(session, ref, closing)) != 0 || closing) {
        __split_internal_unlock(session, parent);
        return (ret);
    }

    /*
     * Split up through the tree as necessary; we're holding the original parent page locked, note
     * the functions we call are responsible for releasing that lock.
     */
    return (__split_parent_climb(session, parent));
}

/*
 * __wt_split_multi --
 *     Split a page into multiple pages.
 */
int
__wt_split_multi(WT_SESSION_IMPL *session, WT_REF *ref, int closing)
{
    WT_DECL_RET;

    __wt_verbose(session, WT_VERB_SPLIT, "%p: split-multi", (void *)ref);

    /*
     * Set the session split generation to ensure underlying code isn't surprised by internal page
     * eviction, then proceed with the split.
     */
    WT_WITH_PAGE_INDEX(session, ret = __split_multi_lock(session, ref, closing));
    return (ret);
}

/*
 * __split_reverse --
 *     Reverse split (rewrite a parent page's index to reflect an empty page).
 */
static int
__split_reverse(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_DECL_RET;
    WT_PAGE *parent;

    /* Lock the parent page, then proceed with the reverse split. */
    WT_RET(__split_internal_lock(session, ref, false, &parent));
    ret = __split_parent(session, ref, NULL, 0, 0, false, true);
    __split_internal_unlock(session, parent);
    return (ret);
}

/*
 * __wt_split_reverse --
 *     Reverse split (rewrite a parent page's index to reflect an empty page).
 */
int
__wt_split_reverse(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_DECL_RET;

    __wt_verbose(session, WT_VERB_SPLIT, "%p: reverse-split", (void *)ref);

    /*
     * Set the session split generation to ensure underlying code isn't surprised by internal page
     * eviction, then proceed with the reverse split.
     */
    WT_WITH_PAGE_INDEX(session, ret = __split_reverse(session, ref));
    return (ret);
}

/*
 * __wt_split_rewrite --
 *     Rewrite an in-memory page with a new version.
 */
int
__wt_split_rewrite(WT_SESSION_IMPL *session, WT_REF *ref, WT_MULTI *multi)
{
    WT_DECL_RET;
    WT_PAGE *page;
    WT_REF *new;

    page = ref->page;

    __wt_verbose(session, WT_VERB_SPLIT, "%p: split-rewrite", (void *)ref);

    /*
     * This isn't a split: a reconciliation failed because we couldn't write something, and in the
     * case of forced eviction, we need to stop this page from being such a problem. We have
     * exclusive access, rewrite the page in memory. The code lives here because the split code
     * knows how to re-create a page in memory after it's been reconciled, and that's exactly what
     * we want to do.
     *
     * Build the new page.
     *
     * Allocate a WT_REF, the error path calls routines that free memory. The only field we need to
     * set is the record number, as it's used by the search routines.
     */
    WT_RET(__wt_calloc_one(session, &new));
    new->ref_recno = ref->ref_recno;

    WT_ERR(__split_multi_inmem(session, page, multi, new));

    /*
     * The rewrite succeeded, we can no longer fail.
     *
     * Finalize the move, discarding moved update lists from the original page.
     */
    __split_multi_inmem_final(session, page, multi);

    /*
     * Discard the original page.
     *
     * Pages with unresolved changes are not marked clean during reconciliation, do it now.
     *
     * Don't count this as eviction making progress, we did a one-for-one rewrite of a page in
     * memory, typical in the case of cache pressure unless the cache is configured for scrub and
     * page doesn't have any skipped updates.
     */
    __wt_page_modify_clear(session, page);
    if (!F_ISSET(S2C(session)->cache, WT_CACHE_EVICT_SCRUB) || multi->supd_restore)
        F_SET_ATOMIC(page, WT_PAGE_EVICT_NO_PROGRESS);
    __wt_ref_out(session, ref);

    /* Swap the new page into place. */
    ref->page = new->page;

    WT_REF_SET_STATE(ref, WT_REF_MEM);

    __wt_free(session, new);
    return (0);

err:
    __split_multi_inmem_fail(session, page, multi, new);
    return (ret);
}
