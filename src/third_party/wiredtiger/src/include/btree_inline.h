/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_ref_is_root --
 *     Return if the page reference is for the root page.
 */
static inline bool
__wt_ref_is_root(WT_REF *ref)
{
    return (ref->home == NULL);
}

/*
 * __wt_ref_cas_state_int --
 *     Try to do a compare and swap, if successful update the ref history in diagnostic mode.
 */
static inline bool
__wt_ref_cas_state_int(WT_SESSION_IMPL *session, WT_REF *ref, uint8_t old_state, uint8_t new_state,
  const char *func, int line)
{
    bool cas_result;

    /* Parameters that are used in a macro for diagnostic builds */
    WT_UNUSED(session);
    WT_UNUSED(func);
    WT_UNUSED(line);

    cas_result = __wt_atomic_casv8(&ref->state, old_state, new_state);

#ifdef HAVE_REF_TRACK
    /*
     * The history update here has potential to race; if the state gets updated again after the CAS
     * above but before the history has been updated.
     */
    if (cas_result)
        WT_REF_SAVE_STATE(ref, new_state, func, line);
#endif
    return (cas_result);
}

/*
 * __wt_btree_disable_bulk --
 *     Disable bulk loads into a tree.
 */
static inline void
__wt_btree_disable_bulk(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;

    btree = S2BT(session);

    /*
     * Once a tree (other than the LSM primary) is no longer empty, eviction should pay attention to
     * it, and it's no longer possible to bulk-load into it.
     */
    if (!btree->original)
        return;
    if (btree->lsm_primary) {
        btree->original = 0; /* Make the next test faster. */
        return;
    }

    /*
     * We use a compare-and-swap here to avoid races among the first inserts into a tree. Eviction
     * is disabled when an empty tree is opened, and it must only be enabled once.
     */
    if (__wt_atomic_cas8(&btree->original, 1, 0)) {
        btree->evict_disabled_open = false;
        __wt_evict_file_exclusive_off(session);
    }
}

/*
 * __wt_page_is_empty --
 *     Return if the page is empty.
 */
static inline bool
__wt_page_is_empty(WT_PAGE *page)
{
    /*
     * Be cautious modifying this function: it's reading fields set by checkpoint reconciliation,
     * and we're not blocking checkpoints (although we must block eviction as it might clear and
     * free these structures).
     */
    return (page->modify != NULL && page->modify->rec_result == WT_PM_REC_EMPTY);
}

/*
 * __wt_page_evict_soon_check --
 *     Check whether the page should be evicted urgently.
 */
static inline bool
__wt_page_evict_soon_check(WT_SESSION_IMPL *session, WT_REF *ref, bool *inmem_split)
{
    WT_BTREE *btree;
    WT_PAGE *page;

    btree = S2BT(session);
    page = ref->page;

    /*
     * Attempt to evict pages with the special "oldest" read generation. This is set for pages that
     * grow larger than the configured memory_page_max setting, when we see many deleted items, and
     * when we are attempting to scan without trashing the cache.
     *
     * Checkpoint should not queue pages for urgent eviction if they require dirty eviction: there
     * is a special exemption that allows checkpoint to evict dirty pages in a tree that is being
     * checkpointed, and no other thread can help with that. Checkpoints don't rely on this code for
     * dirty eviction: that is handled explicitly in __wt_sync_file.
     */
    if (WT_READGEN_EVICT_SOON(page->read_gen) && btree->evict_disabled == 0 &&
      __wt_page_can_evict(session, ref, inmem_split) &&
      (!WT_SESSION_IS_CHECKPOINT(session) || __wt_page_evict_clean(page)))
        return (true);
    return (false);
}

/*
 * __wt_page_evict_clean --
 *     Return if the page can be evicted without dirtying the tree.
 */
static inline bool
__wt_page_evict_clean(WT_PAGE *page)
{
    /*
     * Be cautious modifying this function: it's reading fields set by checkpoint reconciliation,
     * and we're not blocking checkpoints (although we must block eviction as it might clear and
     * free these structures).
     */
    return (page->modify == NULL ||
      (page->modify->page_state == WT_PAGE_CLEAN && page->modify->rec_result == 0));
}

/*
 * __wt_page_is_modified --
 *     Return if the page is dirty.
 */
static inline bool
__wt_page_is_modified(WT_PAGE *page)
{
    /*
     * Be cautious modifying this function: it's reading fields set by checkpoint reconciliation,
     * and we're not blocking checkpoints (although we must block eviction as it might clear and
     * free these structures).
     */
    return (page->modify != NULL && page->modify->page_state != WT_PAGE_CLEAN);
}

/*
 * __wt_btree_block_free --
 *     Helper function to free a block from the current tree.
 */
static inline int
__wt_btree_block_free(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
    WT_BM *bm;
    WT_BTREE *btree;

    btree = S2BT(session);
    bm = btree->bm;

    return (bm->free(bm, session, addr, addr_size));
}

/*
 * __wt_btree_bytes_inuse --
 *     Return the number of bytes in use.
 */
static inline uint64_t
__wt_btree_bytes_inuse(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;
    WT_CACHE *cache;

    btree = S2BT(session);
    cache = S2C(session)->cache;

    return (__wt_cache_bytes_plus_overhead(cache, btree->bytes_inmem));
}

/*
 * __wt_btree_bytes_evictable --
 *     Return the number of bytes that can be evicted (i.e. bytes apart from the pinned root page).
 */
static inline uint64_t
__wt_btree_bytes_evictable(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;
    WT_CACHE *cache;
    WT_PAGE *root_page;
    uint64_t bytes_inmem, bytes_root;

    btree = S2BT(session);
    cache = S2C(session)->cache;
    root_page = btree->root.page;

    bytes_inmem = btree->bytes_inmem;
    bytes_root = root_page == NULL ? 0 : root_page->memory_footprint;

    return (bytes_inmem <= bytes_root ?
        0 :
        __wt_cache_bytes_plus_overhead(cache, bytes_inmem - bytes_root));
}

/*
 * __wt_btree_dirty_inuse --
 *     Return the number of dirty bytes in use.
 */
static inline uint64_t
__wt_btree_dirty_inuse(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;
    WT_CACHE *cache;

    btree = S2BT(session);
    cache = S2C(session)->cache;

    return (
      __wt_cache_bytes_plus_overhead(cache, btree->bytes_dirty_intl + btree->bytes_dirty_leaf));
}

/*
 * __wt_btree_dirty_leaf_inuse --
 *     Return the number of bytes in use by dirty leaf pages.
 */
static inline uint64_t
__wt_btree_dirty_leaf_inuse(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;
    WT_CACHE *cache;

    btree = S2BT(session);
    cache = S2C(session)->cache;

    return (__wt_cache_bytes_plus_overhead(cache, btree->bytes_dirty_leaf));
}

/*
 * __wt_btree_bytes_updates --
 *     Return the number of bytes in use by dirty leaf pages.
 */
static inline uint64_t
__wt_btree_bytes_updates(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;
    WT_CACHE *cache;

    btree = S2BT(session);
    cache = S2C(session)->cache;

    return (__wt_cache_bytes_plus_overhead(cache, btree->bytes_updates));
}

/*
 * __wt_cache_page_inmem_incr --
 *     Increment a page's memory footprint in the cache.
 */
static inline void
__wt_cache_page_inmem_incr(WT_SESSION_IMPL *session, WT_PAGE *page, size_t size)
{
    WT_BTREE *btree;
    WT_CACHE *cache;

    WT_ASSERT(session, size < WT_EXABYTE);
    btree = S2BT(session);
    cache = S2C(session)->cache;

    if (size == 0)
        return;

    /*
     * Always increase the size in sequence of cache, btree, and page as we may race with other
     * threads that are trying to decrease the sizes concurrently.
     */
    (void)__wt_atomic_add64(&cache->bytes_inmem, size);
    (void)__wt_atomic_add64(&btree->bytes_inmem, size);
    if (WT_PAGE_IS_INTERNAL(page)) {
        (void)__wt_atomic_add64(&cache->bytes_internal, size);
        (void)__wt_atomic_add64(&btree->bytes_internal, size);
    }
    (void)__wt_atomic_addsize(&page->memory_footprint, size);

    if (page->modify != NULL) {
        /*
         * If this is an application thread that is running in a txn, keep track of its dirty bytes
         * in the session statistic.
         */
        if (!F_ISSET(session, WT_SESSION_INTERNAL) &&
          F_ISSET(session->txn, WT_TXN_RUNNING | WT_TXN_HAS_ID))
            WT_STAT_SESSION_INCRV(session, txn_bytes_dirty, size);
        if (!WT_PAGE_IS_INTERNAL(page) && !btree->lsm_primary) {
            (void)__wt_atomic_add64(&cache->bytes_updates, size);
            (void)__wt_atomic_add64(&btree->bytes_updates, size);
            (void)__wt_atomic_addsize(&page->modify->bytes_updates, size);
        }
        if (__wt_page_is_modified(page)) {
            if (WT_PAGE_IS_INTERNAL(page)) {
                (void)__wt_atomic_add64(&cache->bytes_dirty_intl, size);
                (void)__wt_atomic_add64(&btree->bytes_dirty_intl, size);
            } else if (!btree->lsm_primary) {
                (void)__wt_atomic_add64(&cache->bytes_dirty_leaf, size);
                (void)__wt_atomic_add64(&btree->bytes_dirty_leaf, size);
            }
            (void)__wt_atomic_addsize(&page->modify->bytes_dirty, size);
        }
    }
}

/*
 * __wt_cache_decr_check_size --
 *     Decrement a size_t cache value and check for underflow.
 */
static inline void
__wt_cache_decr_check_size(WT_SESSION_IMPL *session, size_t *vp, size_t v, const char *fld)
{
    if (v == 0 || __wt_atomic_subsize(vp, v) < WT_EXABYTE)
        return;

    /*
     * It's a bug if this accounting underflowed but allow the application to proceed - the
     * consequence is we use more cache than configured.
     */
    *vp = 0;
    __wt_errx(session, "%s went negative with decrement of %" WT_SIZET_FMT, fld, v);

#ifdef HAVE_DIAGNOSTIC
    __wt_abort(session);
#endif
}

/*
 * __wt_cache_decr_check_uint64 --
 *     Decrement a uint64_t cache value and check for underflow.
 */
static inline void
__wt_cache_decr_check_uint64(WT_SESSION_IMPL *session, uint64_t *vp, uint64_t v, const char *fld)
{
    uint64_t orig = *vp;

    if (v == 0 || __wt_atomic_sub64(vp, v) < WT_EXABYTE)
        return;

    /*
     * It's a bug if this accounting underflowed but allow the application to proceed - the
     * consequence is we use more cache than configured.
     */
    *vp = 0;
    __wt_errx(
      session, "%s was %" PRIu64 ", went negative with decrement of %" PRIu64, fld, orig, v);

#ifdef HAVE_DIAGNOSTIC
    __wt_abort(session);
#endif
}

/*
 * __wt_cache_page_byte_dirty_decr --
 *     Decrement the page's dirty byte count, guarding from underflow.
 */
static inline void
__wt_cache_page_byte_dirty_decr(WT_SESSION_IMPL *session, WT_PAGE *page, size_t size)
{
    WT_BTREE *btree;
    WT_CACHE *cache;
    size_t decr, orig;
    int i;

    btree = S2BT(session);
    cache = S2C(session)->cache;
    decr = 0; /* [-Wconditional-uninitialized] */

    /*
     * We don't have exclusive access and there are ways of decrementing the
     * page's dirty byte count by a too-large value. For example:
     *	T1: __wt_cache_page_inmem_incr(page, size)
     *		page is clean, don't increment dirty byte count
     *	T2: mark page dirty
     *	T1: __wt_cache_page_inmem_decr(page, size)
     *		page is dirty, decrement dirty byte count
     * and, of course, the reverse where the page is dirty at the increment
     * and clean at the decrement.
     *
     * The page's dirty-byte value always reflects bytes represented in the
     * cache's dirty-byte count, decrement the page/cache as much as we can
     * without underflow. If we can't decrement the dirty byte counts after
     * few tries, give up: the cache's value will be wrong, but consistent,
     * and we'll fix it the next time this page is marked clean, or evicted.
     *
     * Always decrease the size in sequence of page, btree, and cache as we may race with other
     * threads that are trying to increase the sizes concurrently.
     */
    for (i = 0; i < 5; ++i) {
        /*
         * Take care to read the dirty-byte count only once in case we're racing with updates.
         */
        WT_ORDERED_READ(orig, page->modify->bytes_dirty);
        decr = WT_MIN(size, orig);
        if (__wt_atomic_cassize(&page->modify->bytes_dirty, orig, orig - decr))
            break;
    }

    if (i == 5)
        return;

    if (WT_PAGE_IS_INTERNAL(page)) {
        __wt_cache_decr_check_uint64(
          session, &btree->bytes_dirty_intl, decr, "WT_BTREE.bytes_dirty_intl");
        __wt_cache_decr_check_uint64(
          session, &cache->bytes_dirty_intl, decr, "WT_CACHE.bytes_dirty_intl");
    } else if (!btree->lsm_primary) {
        __wt_cache_decr_check_uint64(
          session, &btree->bytes_dirty_leaf, decr, "WT_BTREE.bytes_dirty_leaf");
        __wt_cache_decr_check_uint64(
          session, &cache->bytes_dirty_leaf, decr, "WT_CACHE.bytes_dirty_leaf");
    }
}

/*
 * __wt_cache_page_byte_updates_decr --
 *     Decrement the page's update byte count, guarding from underflow.
 */
static inline void
__wt_cache_page_byte_updates_decr(WT_SESSION_IMPL *session, WT_PAGE *page, size_t size)
{
    WT_BTREE *btree;
    WT_CACHE *cache;
    size_t decr, orig;
    int i;

    btree = S2BT(session);
    cache = S2C(session)->cache;
    decr = 0; /* [-Wconditional-uninitialized] */

    WT_ASSERT(session, !WT_PAGE_IS_INTERNAL(page) && !btree->lsm_primary && page->modify != NULL);

    /* See above for why this can race. */
    for (i = 0; i < 5; ++i) {
        WT_ORDERED_READ(orig, page->modify->bytes_updates);
        decr = WT_MIN(size, orig);
        if (__wt_atomic_cassize(&page->modify->bytes_updates, orig, orig - decr))
            break;
    }

    if (i == 5)
        return;

    __wt_cache_decr_check_uint64(session, &btree->bytes_updates, decr, "WT_BTREE.bytes_updates");
    __wt_cache_decr_check_uint64(session, &cache->bytes_updates, decr, "WT_CACHE.bytes_updates");
}
/*
 * __wt_cache_page_inmem_decr --
 *     Decrement a page's memory footprint in the cache.
 */
static inline void
__wt_cache_page_inmem_decr(WT_SESSION_IMPL *session, WT_PAGE *page, size_t size)
{
    WT_BTREE *btree;
    WT_CACHE *cache;

    btree = S2BT(session);
    cache = S2C(session)->cache;

    WT_ASSERT(session, size < WT_EXABYTE);

    /*
     * Always decrease the size in sequence of page, btree, and cache as we may race with other
     * threads that are trying to increase the sizes concurrently.
     */
    __wt_cache_decr_check_size(session, &page->memory_footprint, size, "WT_PAGE.memory_footprint");
    __wt_cache_decr_check_uint64(session, &btree->bytes_inmem, size, "WT_BTREE.bytes_inmem");
    __wt_cache_decr_check_uint64(session, &cache->bytes_inmem, size, "WT_CACHE.bytes_inmem");
    if (page->modify != NULL && !WT_PAGE_IS_INTERNAL(page) && !btree->lsm_primary)
        __wt_cache_page_byte_updates_decr(session, page, size);
    if (__wt_page_is_modified(page))
        __wt_cache_page_byte_dirty_decr(session, page, size);
    /* Track internal size in cache. */
    if (WT_PAGE_IS_INTERNAL(page)) {
        __wt_cache_decr_check_uint64(
          session, &btree->bytes_internal, size, "WT_BTREE.bytes_internal");
        __wt_cache_decr_check_uint64(
          session, &cache->bytes_internal, size, "WT_CACHE.bytes_internal");
    }
}

/*
 * __wt_cache_dirty_incr --
 *     Page switch from clean to dirty: increment the cache dirty page/byte counts.
 */
static inline void
__wt_cache_dirty_incr(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_BTREE *btree;
    WT_CACHE *cache;
    size_t size;

    btree = S2BT(session);
    cache = S2C(session)->cache;

    /*
     * Always increase the size in sequence of cache, btree, and page as we may race with other
     * threads that are trying to decrease the sizes concurrently.
     *
     * Take care to read the memory_footprint once in case we are racing with updates.
     */
    size = page->memory_footprint;
    if (WT_PAGE_IS_INTERNAL(page)) {
        (void)__wt_atomic_add64(&cache->pages_dirty_intl, 1);
        (void)__wt_atomic_add64(&cache->bytes_dirty_intl, size);
        (void)__wt_atomic_add64(&btree->bytes_dirty_intl, size);
    } else {
        if (!btree->lsm_primary) {
            (void)__wt_atomic_add64(&cache->bytes_dirty_leaf, size);
            (void)__wt_atomic_add64(&btree->bytes_dirty_leaf, size);
        }
        (void)__wt_atomic_add64(&cache->pages_dirty_leaf, 1);
    }
    (void)__wt_atomic_add64(&cache->bytes_dirty_total, size);
    (void)__wt_atomic_add64(&btree->bytes_dirty_total, size);
    (void)__wt_atomic_addsize(&page->modify->bytes_dirty, size);
}

/*
 * __wt_cache_dirty_decr --
 *     Page switch from dirty to clean: decrement the cache dirty page/byte counts.
 */
static inline void
__wt_cache_dirty_decr(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_CACHE *cache;
    WT_PAGE_MODIFY *modify;

    cache = S2C(session)->cache;

    if (WT_PAGE_IS_INTERNAL(page))
        __wt_cache_decr_check_uint64(
          session, &cache->pages_dirty_intl, 1, "dirty internal page count");
    else
        __wt_cache_decr_check_uint64(session, &cache->pages_dirty_leaf, 1, "dirty leaf page count");

    modify = page->modify;
    if (modify != NULL && modify->bytes_dirty != 0)
        __wt_cache_page_byte_dirty_decr(session, page, modify->bytes_dirty);
}

/*
 * __wt_cache_page_image_decr --
 *     Decrement a page image's size to the cache.
 */
static inline void
__wt_cache_page_image_decr(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_CACHE *cache;

    cache = S2C(session)->cache;

    if (WT_PAGE_IS_INTERNAL(page))
        __wt_cache_decr_check_uint64(
          session, &cache->bytes_image_intl, page->dsk->mem_size, "WT_CACHE.bytes_image");
    else
        __wt_cache_decr_check_uint64(
          session, &cache->bytes_image_leaf, page->dsk->mem_size, "WT_CACHE.bytes_image");
}

/*
 * __wt_cache_page_image_incr --
 *     Increment a page image's size to the cache.
 */
static inline void
__wt_cache_page_image_incr(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_CACHE *cache;

    cache = S2C(session)->cache;
    if (WT_PAGE_IS_INTERNAL(page))
        (void)__wt_atomic_add64(&cache->bytes_image_intl, page->dsk->mem_size);
    else
        (void)__wt_atomic_add64(&cache->bytes_image_leaf, page->dsk->mem_size);
}

/*
 * __wt_cache_page_evict --
 *     Evict pages from the cache.
 */
static inline void
__wt_cache_page_evict(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_BTREE *btree;
    WT_CACHE *cache;
    WT_PAGE_MODIFY *modify;

    btree = S2BT(session);
    cache = S2C(session)->cache;
    modify = page->modify;

    /* Update the bytes in-memory to reflect the eviction. */
    __wt_cache_decr_check_uint64(
      session, &btree->bytes_inmem, page->memory_footprint, "WT_BTREE.bytes_inmem");
    __wt_cache_decr_check_uint64(
      session, &cache->bytes_inmem, page->memory_footprint, "WT_CACHE.bytes_inmem");

    /* Update the bytes_internal value to reflect the eviction */
    if (WT_PAGE_IS_INTERNAL(page)) {
        __wt_cache_decr_check_uint64(
          session, &btree->bytes_internal, page->memory_footprint, "WT_BTREE.bytes_internal");
        __wt_cache_decr_check_uint64(
          session, &cache->bytes_internal, page->memory_footprint, "WT_CACHE.bytes_internal");
    }

    /* Update the cache's dirty-byte count. */
    if (modify != NULL && modify->bytes_dirty != 0) {
        if (WT_PAGE_IS_INTERNAL(page)) {
            __wt_cache_decr_check_uint64(
              session, &btree->bytes_dirty_intl, modify->bytes_dirty, "WT_BTREE.bytes_dirty_intl");
            __wt_cache_decr_check_uint64(
              session, &cache->bytes_dirty_intl, modify->bytes_dirty, "WT_CACHE.bytes_dirty_intl");
        } else if (!btree->lsm_primary) {
            __wt_cache_decr_check_uint64(
              session, &btree->bytes_dirty_leaf, modify->bytes_dirty, "WT_BTREE.bytes_dirty_leaf");
            __wt_cache_decr_check_uint64(
              session, &cache->bytes_dirty_leaf, modify->bytes_dirty, "WT_CACHE.bytes_dirty_leaf");
        }
    }

    /* Update the cache's updates-byte count. */
    if (modify != NULL) {
        __wt_cache_decr_check_uint64(
          session, &btree->bytes_updates, modify->bytes_updates, "WT_BTREE.bytes_updates");
        __wt_cache_decr_check_uint64(
          session, &cache->bytes_updates, modify->bytes_updates, "WT_CACHE.bytes_updates");
    }

    /* Update bytes and pages evicted. */
    (void)__wt_atomic_add64(&cache->bytes_evict, page->memory_footprint);
    (void)__wt_atomic_addv64(&cache->pages_evicted, 1);

    /*
     * Track if eviction makes progress. This is used in various places to determine whether
     * eviction is stuck.
     */
    if (!F_ISSET_ATOMIC_16(page, WT_PAGE_EVICT_NO_PROGRESS))
        (void)__wt_atomic_addv64(&cache->eviction_progress, 1);
}

/*
 * __wt_update_list_memsize --
 *     The size in memory of a list of updates.
 */
static inline size_t
__wt_update_list_memsize(WT_UPDATE *upd)
{
    size_t upd_size;

    for (upd_size = 0; upd != NULL; upd = upd->next)
        upd_size += WT_UPDATE_MEMSIZE(upd);

    return (upd_size);
}

/*
 * __wt_page_modify_init --
 *     A page is about to be modified, allocate the modification structure.
 */
static inline int
__wt_page_modify_init(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    return (page->modify == NULL ? __wt_page_modify_alloc(session, page) : 0);
}

/*
 * __wt_page_only_modify_set --
 *     Mark the page (but only the page) dirty.
 */
static inline void
__wt_page_only_modify_set(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    uint64_t last_running;

    WT_ASSERT(session, !F_ISSET(session->dhandle, WT_DHANDLE_DEAD));

    last_running = 0;
    if (page->modify->page_state == WT_PAGE_CLEAN)
        last_running = S2C(session)->txn_global.last_running;

    /*
     * We depend on the atomic operation being a write barrier, that is, a barrier to ensure all
     * changes to the page are flushed before updating the page state and/or marking the tree dirty,
     * otherwise checkpoints and/or page reconciliation might be looking at a clean page/tree.
     *
     * Every time the page transitions from clean to dirty, update the cache and transactional
     * information.
     *
     * The page state can only ever be incremented above dirty by the number of concurrently running
     * threads, so the counter will never approach the point where it would wrap.
     */
    if (page->modify->page_state < WT_PAGE_DIRTY &&
      __wt_atomic_add32(&page->modify->page_state, 1) == WT_PAGE_DIRTY_FIRST) {
        __wt_cache_dirty_incr(session, page);
        /*
         * In the event we dirty a page which is flagged for eviction soon, we update its read
         * generation to avoid evicting a dirty page prematurely.
         */
        if (page->read_gen == WT_READGEN_WONT_NEED)
            __wt_cache_read_gen_new(session, page);

        /*
         * We won the race to dirty the page, but another thread could have committed in the
         * meantime, and the last_running field been updated past it. That is all very unlikely, but
         * not impossible, so we take care to read the global state before the atomic increment.
         *
         * If the page was dirty on entry, then last_running == 0. The page could have become clean
         * since then, if reconciliation completed. In that case, we leave the previous value for
         * first_dirty_txn rather than potentially racing to update it, at worst, we'll
         * unnecessarily write a page in a checkpoint.
         */
        if (last_running != 0)
            page->modify->first_dirty_txn = last_running;
    }

    /* Check if this is the largest transaction ID to update the page. */
    if (WT_TXNID_LT(page->modify->update_txn, session->txn->id))
        page->modify->update_txn = session->txn->id;
}

/*
 * __wt_tree_modify_set --
 *     Mark the tree dirty.
 */
static inline void
__wt_tree_modify_set(WT_SESSION_IMPL *session)
{
    /*
     * Test before setting the dirty flag, it's a hot cache line.
     *
     * The tree's modified flag is cleared by the checkpoint thread: set it and insert a barrier
     * before dirtying the page. (I don't think it's a problem if the tree is marked dirty with all
     * the pages clean, it might result in an extra checkpoint that doesn't do any work but it
     * shouldn't cause problems; regardless, let's play it safe.)
     */
    if (!S2BT(session)->modified) {
        /* Assert we never dirty a checkpoint handle. */
        WT_ASSERT(session, !WT_READING_CHECKPOINT(session));

        S2BT(session)->modified = true;
        WT_FULL_BARRIER();

        /*
         * There is a potential race where checkpoint walks the tree and marks it as clean before a
         * page is subsequently marked as dirty, leaving us with a dirty page on a clean tree. Yield
         * here to encourage this scenario and ensure we're handling it correctly.
         */
        WT_DIAGNOSTIC_YIELD;
    }

    /*
     * The btree may already be marked dirty while the connection is still clean; mark the
     * connection dirty outside the test of the btree state.
     */
    if (!S2C(session)->modified)
        S2C(session)->modified = true;
}

/*
 * __wt_page_modify_clear --
 *     Clean a modified page.
 */
static inline void
__wt_page_modify_clear(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    /*
     * The page must be held exclusive when this call is made, this call can only be used when the
     * page is owned by a single thread.
     *
     * Allow the call to be made on clean pages.
     */
    if (__wt_page_is_modified(page)) {
        /*
         * The only part where ordering matters is during reconciliation where updates on other
         * threads are performing writes to the page state that need to be visible to the
         * reconciliation thread.
         *
         * Since clearing of the page state is not going to be happening during reconciliation on a
         * separate thread, there's no write barrier needed here.
         */
        page->modify->page_state = WT_PAGE_CLEAN;
        __wt_cache_dirty_decr(session, page);
    }
}

/*
 * __wt_page_modify_set --
 *     Mark the page and tree dirty.
 */
static inline void
__wt_page_modify_set(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    /*
     * Prepared records in the datastore require page updates, even for read-only handles, don't
     * mark the tree or page dirty.
     */
    if (F_ISSET(S2BT(session), WT_BTREE_READONLY))
        return;

    /*
     * Mark the tree dirty (even if the page is already marked dirty), newly created pages to
     * support "empty" files are dirty, but the file isn't marked dirty until there's a real change
     * needing to be written.
     */
    __wt_tree_modify_set(session);

    __wt_page_only_modify_set(session, page);

    /*
     * We need to make sure a checkpoint doesn't come through and mark the tree clean before we have
     * a chance to mark the page dirty. Otherwise, the checkpoint may also visit the page before it
     * is marked dirty and skip it without also marking the tree clean. Worst case scenario with
     * this approach is that a future checkpoint reviews the tree again unnecessarily - however, it
     * is likely this is necessary since the update triggering this modify set would not be included
     * in the checkpoint. If hypothetically a checkpoint came through after the page was modified
     * and before the tree is marked dirty again, that is fine. The transaction installing this
     * update wasn't visible to the checkpoint, so it's reasonable for the tree to remain dirty.
     */
    __wt_tree_modify_set(session);
}

/*
 * __wt_page_parent_modify_set --
 *     Mark the parent page, and optionally the tree, dirty.
 */
static inline int
__wt_page_parent_modify_set(WT_SESSION_IMPL *session, WT_REF *ref, bool page_only)
{
    WT_PAGE *parent;

    /*
     * This function exists as a place to stash this comment. There are a few places where we need
     * to dirty a page's parent. The trick is the page's parent might split at any point, and the
     * page parent might be the wrong parent at any particular time. We ignore this and dirty
     * whatever page the page's reference structure points to. This is safe because if we're
     * pointing to the wrong parent, that parent must have split, deepening the tree, which implies
     * marking the original parent and all of the newly-created children as dirty. In other words,
     * if we have the wrong parent page, everything was marked dirty already.
     */
    parent = ref->home;
    WT_RET(__wt_page_modify_init(session, parent));
    if (page_only)
        __wt_page_only_modify_set(session, parent);
    else
        __wt_page_modify_set(session, parent);
    return (0);
}

/*
 * __wt_off_page --
 *     Return if a pointer references off-page data.
 */
static inline bool
__wt_off_page(WT_PAGE *page, const void *p)
{
    /*
     * There may be no underlying page, in which case the reference is off-page by definition.
     */
    return (page->dsk == NULL || p < (void *)page->dsk ||
      p >= (void *)((uint8_t *)page->dsk + page->dsk->mem_size));
}

/*
 * __wt_ref_key --
 *     Return a reference to a row-store internal page key as cheaply as possible.
 */
static inline void
__wt_ref_key(WT_PAGE *page, WT_REF *ref, void *keyp, size_t *sizep)
{
    uintptr_t v;

/*
 * An internal page key is in one of two places: if we instantiated the
 * key (for example, when reading the page), WT_REF.ref_ikey references
 * a WT_IKEY structure, otherwise WT_REF.ref_ikey references an on-page
 * key offset/length pair.
 *
 * Now the magic: allocated memory must be aligned to store any standard
 * type, and we expect some standard type to require at least quad-byte
 * alignment, so allocated memory should have some clear low-order bits.
 * On-page objects consist of an offset/length pair: the maximum page
 * size currently fits into 29 bits, so we use the low-order bits of the
 * pointer to mark the other bits of the pointer as encoding the key's
 * location and length.  This breaks if allocated memory isn't aligned,
 * of course.
 *
 * In this specific case, we use bit 0x01 to mark an on-page key, else
 * it's a WT_IKEY reference.  The bit pattern for internal row-store
 * on-page keys is:
 *	32 bits		key length
 *	31 bits		page offset of the key's bytes,
 *	 1 bits		flags
 */
#define WT_IK_FLAG 0x01
#define WT_IK_ENCODE_KEY_LEN(v) ((uintptr_t)(v) << 32)
#define WT_IK_DECODE_KEY_LEN(v) ((v) >> 32)
#define WT_IK_ENCODE_KEY_OFFSET(v) ((uintptr_t)(v) << 1)
#define WT_IK_DECODE_KEY_OFFSET(v) (((v)&0xFFFFFFFF) >> 1)
    v = (uintptr_t)ref->ref_ikey;
    if (v & WT_IK_FLAG) {
        *(void **)keyp = WT_PAGE_REF_OFFSET(page, WT_IK_DECODE_KEY_OFFSET(v));
        *sizep = WT_IK_DECODE_KEY_LEN(v);
    } else {
        *(void **)keyp = WT_IKEY_DATA(ref->ref_ikey);
        *sizep = ((WT_IKEY *)ref->ref_ikey)->size;
    }
}

/*
 * __wt_ref_key_onpage_set --
 *     Set a WT_REF to reference an on-page key.
 */
static inline void
__wt_ref_key_onpage_set(WT_PAGE *page, WT_REF *ref, WT_CELL_UNPACK_ADDR *unpack)
{
    uintptr_t v;

    /*
     * See the comment in __wt_ref_key for an explanation of the magic.
     */
    v = WT_IK_ENCODE_KEY_LEN(unpack->size) |
      WT_IK_ENCODE_KEY_OFFSET(WT_PAGE_DISK_OFFSET(page, unpack->data)) | WT_IK_FLAG;
    ref->ref_ikey = (void *)v;
}

/*
 * __wt_ref_key_instantiated --
 *     Return if a WT_REF key is instantiated.
 */
static inline WT_IKEY *
__wt_ref_key_instantiated(WT_REF *ref)
{
    uintptr_t v;

    /*
     * See the comment in __wt_ref_key for an explanation of the magic.
     */
    v = (uintptr_t)ref->ref_ikey;
    return (v & WT_IK_FLAG ? NULL : (WT_IKEY *)ref->ref_ikey);
}

/*
 * __wt_ref_key_clear --
 *     Clear a WT_REF key.
 */
static inline void
__wt_ref_key_clear(WT_REF *ref)
{
    /*
     * The key union has 2 8B fields; this is equivalent to:
     *
     *	ref->ref_recno = WT_RECNO_OOB;
     *	ref->ref_ikey = NULL;
     */
    ref->ref_recno = 0;
}

/*
 * __wt_row_leaf_key_info --
 *     Return a row-store leaf page key referenced by a WT_ROW if it can be had without unpacking a
 *     cell, and information about the cell, if the key isn't cheaply available.
 */
static inline void
__wt_row_leaf_key_info(WT_PAGE *page, void *copy, WT_IKEY **ikeyp, WT_CELL **cellp, void *datap,
  size_t *sizep, uint8_t *prefixp)
{
    WT_IKEY *ikey;
    uintptr_t v;

    v = (uintptr_t)copy;

    /*
     * A row-store leaf page key is in one of two places: if instantiated, the WT_ROW pointer
     * references a WT_IKEY structure, otherwise, it references an on-page item. Further, on-page
     * items are in one of two states: if the key is a simple key (not an overflow key, which is
     * likely), the key's offset, size and prefix is encoded in the 8B of pointer. Otherwise, the
     * offset is to the key's on-page cell.
     *
     * This function returns information from a set of things about the key (WT_IKEY reference, cell
     * reference and/or key/length/prefix triplet). Our callers know the order we resolve items and
     * what information will be returned. Specifically, the caller gets a key (in the form of a
     * pointer to the bytes, a length and a prefix length in all cases where we can get it without
     * unpacking a cell), plus an optional WT_IKEY reference, and in all cases, a pointer to the
     * on-page cell. Our caller's test is generally if there is a returned key or not, falling back
     * to the returned cell.
     *
     * Now the magic: allocated memory must be aligned to store any standard type and we expect some
     * standard type to require at least quad-byte alignment, so allocated memory should have two
     * clear low-order bits. On-page objects consist of an offset/length pair and a prefix in the
     * case of a key: the maximum page size is 29 bits (512MB), the remaining bits hold the key or
     * value location and bytes. This breaks if allocated memory isn't aligned, of course.
     *
     * In this specific case, we use bit 0x01 to mark an on-page cell, bit 0x02 to mark an on-page
     * key, 0x03 to mark an on-page key/value pair, otherwise it's a WT_IKEY reference. The bit
     * pattern for on-page cells is:
     *
     *  29 bits		offset of the key's cell (512MB)
     *   2 bits		0x01 flag
     *
     * The on-page cell is our fallback: if a key or value won't fit into our encoding (unlikely,
     * but possible), we fall back to using a cell reference, which obviously has enough room for
     * all possible values.
     *
     * The next encoding is for on-page keys:
     *
     *  19 bits		key's length (512KB)
     *   6 bits		offset of the key's bytes from the key's cell (32B)
     *   8 bits		key's prefix length (256B, the maximum possible value)
     *  29 bits		offset of the key's cell (512MB)
     *   2 bits		0x02 flag
     *
     * But, while that allows us to skip decoding simple key cells, we also want to skip decoding
     * value cells in the case where the value cell is also simple/short. We use bit 0x03 to mark
     * an encoded on-page key and value pair. The encoding for on-page key/value pairs is:
     *
     *  13 bits		value's length (8KB)
     *   6 bits		offset of the value's bytes from the end of the key's cell (32B)
     *  12 bits		key's length (4KB)
     *   6 bits		offset of the key's bytes from the key's cell (32B)
     *   8 bits		key's prefix length (256B, the maximum possible value)
     *  17 bits		offset of the key's cell (128KB)
     *   2 bits		0x03 flag
     *
     * A reason for the complexity here is we need to be able to find the key and value cells from
     * the encoded form: for that reason we store an offset to the key cell plus a second offset to
     * the start of the key's bytes. Finding the value cell is reasonably straight-forward, we use
     * the location of the key to find the cell immediately following the key.
     *
     * A simple extension of this encoding would be to encode zero-length values similarly to how we
     * encode short values. However, zero-length values are noted by adjacent key cells on the page,
     * and we detect that without decoding the second cell by checking the cell's type byte. Tests
     * indicate it's slightly slower to encode missing value cells than to check the cell type, so
     * we don't bother with the encoding.
     *
     * Generally, the bitfields are expected to be larger than the stored items (4/8KB keys/values,
     * 128KB pages), but the underlying limits are larger and we can see items we cannot encode in
     * this way.  For example, if an application creates pages larger than 128KB, encoded key/value
     * offsets after the maximum offset (the offsets of cells at the end of the page), couldn't be
     * encoded. If that's not working, these bit patterns can be changed as they are in-memory only
     * (we could even tune for specific workloads in specific trees).
     */
#define WT_KEY_FLAG_BITS 0x03

#define WT_CELL_FLAG 0x01
/* key cell offset field size can hold maximum value, WT_CELL_MAX_KEY_CELL_OFFSET not needed. */
#define WT_CELL_ENCODE_OFFSET(v) ((uintptr_t)(v) << 2)
#define WT_CELL_DECODE_OFFSET(v) ((v) >> 2)

#define WT_K_FLAG 0x02
#define WT_K_MAX_KEY_LEN (0x80000 - 1)
#define WT_K_DECODE_KEY_LEN(v) (((v)&0xffffe00000000000) >> 45)
#define WT_K_ENCODE_KEY_LEN(v) ((uintptr_t)(v) << 45)
#define WT_K_MAX_KEY_OFFSET (0x40 - 1)
#define WT_K_DECODE_KEY_OFFSET(v) (((v)&0x001f8000000000) >> 39)
#define WT_K_ENCODE_KEY_OFFSET(v) ((uintptr_t)(v) << 39)
/* Key prefix field size can hold maximum value, WT_K_MAX_KEY_PREFIX not needed. */
#define WT_K_DECODE_KEY_PREFIX(v) (((v)&0x00007f80000000) >> 31)
#define WT_K_ENCODE_KEY_PREFIX(v) ((uintptr_t)(v) << 31)
/* Key cell offset field size can hold maximum value, WT_K_MAX_KEY_CELL_OFFSET not needed. */
#define WT_K_DECODE_KEY_CELL_OFFSET(v) (((v)&0x0000007ffffffc) >> 2)
#define WT_K_ENCODE_KEY_CELL_OFFSET(v) ((uintptr_t)(v) << 2)

#define WT_KV_FLAG 0x03
#define WT_KV_MAX_VALUE_LEN (0x2000 - 1)
#define WT_KV_DECODE_VALUE_LEN(v) (((v)&0xfff8000000000000) >> 51)
#define WT_KV_ENCODE_VALUE_LEN(v) ((uintptr_t)(v) << 51)
#define WT_KV_MAX_VALUE_OFFSET (0x40 - 1)
#define WT_KV_DECODE_VALUE_OFFSET(v) (((v)&0x07e00000000000) >> 45)
#define WT_KV_ENCODE_VALUE_OFFSET(v) ((uintptr_t)(v) << 45)
#define WT_KV_MAX_KEY_LEN (0x1000 - 1)
#define WT_KV_DECODE_KEY_LEN(v) (((v)&0x001ffe00000000) >> 33)
#define WT_KV_ENCODE_KEY_LEN(v) ((uintptr_t)(v) << 33)
/* Key offset encoding is the same for key and key/value forms, WT_KV_MAX_KEY_OFFSET not needed. */
#define WT_KV_DECODE_KEY_OFFSET(v) (((v)&0x000001f8000000) >> 27)
#define WT_KV_ENCODE_KEY_OFFSET(v) ((uintptr_t)(v) << 27)
/* Key prefix encoding is the same for key and key/value forms, WT_KV_MAX_KEY_PREFIX not needed. */
#define WT_KV_DECODE_KEY_PREFIX(v) (((v)&0x00000007f80000) >> 19)
#define WT_KV_ENCODE_KEY_PREFIX(v) ((uintptr_t)(v) << 19)
#define WT_KV_MAX_KEY_CELL_OFFSET (0x20000 - 1)
#define WT_KV_DECODE_KEY_CELL_OFFSET(v) (((v)&0x0000000007fffc) >> 2)
#define WT_KV_ENCODE_KEY_CELL_OFFSET(v) ((uintptr_t)(v) << 2)

    switch (v & WT_KEY_FLAG_BITS) {
    case WT_CELL_FLAG: /* On-page cell. */
        if (ikeyp != NULL)
            *ikeyp = NULL;
        if (cellp != NULL)
            *cellp = (WT_CELL *)WT_PAGE_REF_OFFSET(page, WT_CELL_DECODE_OFFSET(v));
        if (datap != NULL) {
            *(void **)datap = NULL;
            *sizep = 0;
            *prefixp = 0;
        }
        break;
    case WT_K_FLAG: /* Encoded key. */
        if (ikeyp != NULL)
            *ikeyp = NULL;
        if (cellp != NULL)
            *cellp = (WT_CELL *)WT_PAGE_REF_OFFSET(page, WT_K_DECODE_KEY_CELL_OFFSET(v));
        if (datap != NULL) {
            *(void **)datap =
              WT_PAGE_REF_OFFSET(page, WT_K_DECODE_KEY_CELL_OFFSET(v) + WT_K_DECODE_KEY_OFFSET(v));
            *sizep = WT_K_DECODE_KEY_LEN(v);
            *prefixp = (uint8_t)WT_K_DECODE_KEY_PREFIX(v);
        }
        break;
    case WT_KV_FLAG: /* Encoded key/value pair. */
        if (ikeyp != NULL)
            *ikeyp = NULL;
        if (cellp != NULL)
            *cellp = (WT_CELL *)WT_PAGE_REF_OFFSET(page, WT_KV_DECODE_KEY_CELL_OFFSET(v));
        if (datap != NULL) {
            *(void **)datap = WT_PAGE_REF_OFFSET(
              page, WT_KV_DECODE_KEY_CELL_OFFSET(v) + WT_KV_DECODE_KEY_OFFSET(v));
            *sizep = WT_KV_DECODE_KEY_LEN(v);
            *prefixp = (uint8_t)WT_KV_DECODE_KEY_PREFIX(v);
        }
        break;
    default: /* Instantiated key. */
        ikey = (WT_IKEY *)copy;
        if (ikeyp != NULL)
            *ikeyp = ikey;
        if (cellp != NULL)
            *cellp = ikey->cell_offset == 0 ?
              NULL :
              (WT_CELL *)WT_PAGE_REF_OFFSET(page, ikey->cell_offset);
        if (datap != NULL) {
            *(void **)datap = WT_IKEY_DATA(ikey);
            *sizep = ikey->size;
            *prefixp = 0;
        }
        break;
    }
}

/*
 * __wt_row_leaf_key_set --
 *     Set a WT_ROW to reference an on-page row-store leaf key.
 */
static inline void
__wt_row_leaf_key_set(WT_PAGE *page, WT_ROW *rip, WT_CELL_UNPACK_KV *unpack)
{
    uintptr_t key_offset, v;

    /*
     * See the comment in __wt_row_leaf_key_info for an explanation of the magic.
     *
     * Not checking the prefix and cell offset sizes, the fields hold any legitimate value.
     */
    key_offset = (uintptr_t)WT_PTRDIFF(unpack->data, unpack->cell);
    if (unpack->type != WT_CELL_KEY || key_offset > WT_K_MAX_KEY_OFFSET ||
      unpack->size > WT_K_MAX_KEY_LEN)
        v = WT_CELL_ENCODE_OFFSET(WT_PAGE_DISK_OFFSET(page, unpack->cell)) | WT_CELL_FLAG;
    else
        v = WT_K_ENCODE_KEY_CELL_OFFSET(WT_PAGE_DISK_OFFSET(page, unpack->cell)) |
          WT_K_ENCODE_KEY_PREFIX(unpack->prefix) | WT_K_ENCODE_KEY_OFFSET(key_offset) |
          WT_K_ENCODE_KEY_LEN(unpack->size) | WT_K_FLAG;

    WT_ROW_KEY_SET(rip, v);
}

/*
 * __wt_row_leaf_value_set --
 *     Set a WT_ROW to reference an on-page row-store leaf key and value pair, if possible.
 */
static inline void
__wt_row_leaf_value_set(WT_ROW *rip, WT_CELL_UNPACK_KV *unpack)
{
    uintptr_t value_offset, value_size, v;

    /* The row-store key can change underfoot; explicitly take a copy. */
    v = (uintptr_t)WT_ROW_KEY_COPY(rip);

    /*
     * See the comment in __wt_row_leaf_key_info for an explanation of the magic.
     *
     * Only encoded keys can be upgraded to encoded key/value pairs.
     */
    if ((v & WT_KEY_FLAG_BITS) != WT_K_FLAG)
        return;

    if (WT_K_DECODE_KEY_CELL_OFFSET(v) > WT_KV_MAX_KEY_CELL_OFFSET) /* Key cell offset */
        return;
    /*
     * Not checking the prefix size, the field sizes are the same in both encodings.
     *
     * Not checking the key offset, the field sizes are the same in both encodings.
     */
    if (WT_K_DECODE_KEY_LEN(v) > WT_KV_MAX_KEY_LEN) /* Key len */
        return;

    value_offset = (uintptr_t)WT_PTRDIFF(unpack->data, unpack->cell);
    if (value_offset > WT_KV_MAX_VALUE_OFFSET) /* Value offset */
        return;
    value_size = unpack->size;
    if (value_size > WT_KV_MAX_VALUE_LEN) /* Value length */
        return;

    v = WT_KV_ENCODE_KEY_CELL_OFFSET(WT_K_DECODE_KEY_CELL_OFFSET(v)) |
      WT_KV_ENCODE_KEY_PREFIX(WT_K_DECODE_KEY_PREFIX(v)) |
      WT_KV_ENCODE_KEY_OFFSET(WT_K_DECODE_KEY_OFFSET(v)) |
      WT_KV_ENCODE_KEY_LEN(WT_K_DECODE_KEY_LEN(v)) | WT_KV_ENCODE_VALUE_OFFSET(value_offset) |
      WT_KV_ENCODE_VALUE_LEN(value_size) | WT_KV_FLAG;
    WT_ROW_KEY_SET(rip, v);
}

/*
 * __wt_row_leaf_key_free --
 *     Discard any memory allocated for an instantiated key.
 */
static inline void
__wt_row_leaf_key_free(WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip)
{
    WT_IKEY *ikey;
    void *copy;

    /* The row-store key can change underfoot; explicitly take a copy. */
    copy = WT_ROW_KEY_COPY(rip);

    /*
     * If the key was a WT_IKEY allocation (that is, if it points somewhere other than the original
     * page), free the memory.
     */
    __wt_row_leaf_key_info(page, copy, &ikey, NULL, NULL, NULL, NULL);
    __wt_free(session, ikey);
}

/*
 * __wt_row_leaf_key --
 *     Set a buffer to reference a row-store leaf page key as cheaply as possible.
 */
static inline int
__wt_row_leaf_key(
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip, WT_ITEM *key, bool instantiate)
{
    WT_CELL *cell;
    size_t group_size, key_size;
    uint32_t slot;
    uint8_t group_prefix, key_prefix;
    void *copy;
    const void *group_key, *key_data;

    /*
     * A front-end for __wt_row_leaf_key_work, here to inline fast paths.
     *
     * The row-store key can change underfoot; explicitly take a copy.
     */
    copy = WT_ROW_KEY_COPY(rip);

    /*
     * Handle keys taken directly from the disk image (which should be a common case), instantiated
     * keys (rare initially, but possibly more common as leaf page search instantiates keys), and
     * keys built using the most-used page key prefix.
     *
     * The most-used page key prefix: the longest group of compressed key prefixes on the page that
     * can be built from a single, fully instantiated key on the page, was tracked when the page was
     * read. Build keys in that group by appending the key's bytes to the root key from which it was
     * compressed.
     */
    __wt_row_leaf_key_info(page, copy, NULL, &cell, &key_data, &key_size, &key_prefix);
    if (key_data != NULL && key_prefix == 0) {
        key->data = key_data;
        key->size = key_size;
        return (0);
    }
    slot = WT_ROW_SLOT(page, rip);
    if (key_data != NULL && slot > page->prefix_start && slot <= page->prefix_stop) {
        /* The row-store key can change underfoot; explicitly take a copy. */
        copy = WT_ROW_KEY_COPY(&page->pg_row[page->prefix_start]);
        __wt_row_leaf_key_info(page, copy, NULL, NULL, &group_key, &group_size, &group_prefix);
        if (group_key != NULL) {
            WT_RET(__wt_buf_init(session, key, key_prefix + key_size));
            memcpy(key->mem, group_key, key_prefix);
            memcpy((uint8_t *)key->mem + key_prefix, key_data, key_size);
            key->size = key_prefix + key_size;
            return (0);
        }
    }

    /*
     * The alternative is an on-page cell with some kind of compressed or overflow key that's never
     * been instantiated. Call the underlying worker function to figure it out.
     */
    return (__wt_row_leaf_key_work(session, page, rip, key, instantiate));
}

/*
 * __wt_row_leaf_key_instantiate --
 *     Instantiate the keys on a leaf page as needed.
 */
static inline int
__wt_row_leaf_key_instantiate(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_CELL *cell;
    WT_DECL_ITEM(key);
    WT_DECL_RET;
    WT_ROW *rip;
    size_t key_size;
    uint32_t i, slot;
    uint8_t key_prefix;
    u_int skip;
    void *copy;
    const void *key_data;

    /*
     * Cursor previous traversals will be too slow in the case of a set of prefix-compressed keys
     * requiring long roll-forward processing. In the worst case, each key would require processing
     * every key appearing before it on the page as we walk backwards through the page. If we're
     * doing a cursor previous call, and this page has never been checked for excessively long
     * stretches of prefix-compressed keys, do it now.
     */
    if (F_ISSET_ATOMIC_16(page, WT_PAGE_BUILD_KEYS))
        return (0);
    F_SET_ATOMIC_16(page, WT_PAGE_BUILD_KEYS);

    /* Walk the keys, making sure there's something easy to work with periodically. */
    skip = 0;
    WT_ROW_FOREACH (page, rip, i) {
        /*
         * Get the key's information. The row-store key can change underfoot; explicitly take a
         * copy.
         */
        copy = WT_ROW_KEY_COPY(rip);
        __wt_row_leaf_key_info(page, copy, NULL, &cell, &key_data, &key_size, &key_prefix);

        /*
         * If the key isn't prefix compressed, or is a prefix-compressed key we can derive from the
         * group record, we're done.
         */
        slot = WT_ROW_SLOT(page, rip);
        if (key_data != NULL &&
          (key_prefix == 0 || (slot > page->prefix_start && slot <= page->prefix_stop))) {
            skip = 0;
            continue;
        }

        /*
         * Skip overflow keys: we'll instantiate them on demand and they don't require any special
         * processing (but they don't help with long strings of prefix compressed keys, either, so
         * we'll likely want to instantiate the first key we find after a long stretch of overflow
         * keys). More importantly, we don't want to instantiate them for a cursor traversal, we
         * only want to instantiate them for a tree search, as that's likely to happen repeatedly.
         */
        if (__wt_cell_type(cell) == WT_CELL_KEY_OVFL) {
            ++skip;
            continue;
        }

        /*
         * If we skip 10 keys, instantiate one, limiting how far we're forced to roll backward. (The
         * value 10 was chosen for no particular reason.) There are still cases where we might not
         * need to instantiate this key (for example, a key too large to be encoded, but still
         * on-page and not prefix-compressed). Let the underlying worker function figure that out,
         * we should have found the vast majority of cases by now.
         */
        if (++skip >= 10) {
            if (key == NULL)
                WT_ERR(__wt_scr_alloc(session, 0, &key));
            WT_ERR(__wt_row_leaf_key(session, page, rip, key, true));
            skip = 0;
        }
    }

err:
    __wt_scr_free(session, &key);
    return (ret);
}

/*
 * __wt_row_leaf_value_is_encoded --
 *     Return if the value for a row-store leaf page is an encoded key/value pair.
 */
static inline bool
__wt_row_leaf_value_is_encoded(WT_ROW *rip)
{
    uintptr_t v;

    /* The row-store key can change underfoot; explicitly take a copy. */
    v = (uintptr_t)WT_ROW_KEY_COPY(rip);

    /*
     * See the comment in __wt_row_leaf_key_info for an explanation of the magic.
     */
    return ((v & WT_KEY_FLAG_BITS) == WT_KV_FLAG);
}

/*
 * __wt_row_leaf_value --
 *     Return the value for a row-store leaf page encoded key/value pair.
 */
static inline bool
__wt_row_leaf_value(WT_PAGE *page, WT_ROW *rip, WT_ITEM *value)
{
    uintptr_t v;

    /* The row-store key can change underfoot; explicitly take a copy. */
    v = (uintptr_t)WT_ROW_KEY_COPY(rip);

    if ((v & WT_KEY_FLAG_BITS) == WT_KV_FLAG) {
        /*
         * See the comment in __wt_row_leaf_key_info for an explanation of the magic.
         *
         * Normally a value is represented by the value's cell in the disk image (or an update), but
         * there is a fast path for returning a simple value, where it's worth the additional effort
         * of encoding the value in the per-row reference and retrieving it. This function does that
         * work, while most value retrieval goes through the "return the unpacked cell" version.
         *
         * The value's data is the page offset of the key's cell, plus the key's offset, plus the
         * key's size, plus the value's offset: in other words, we know where the key's cell starts,
         * the key's data ends the key's cell, and the value cell immediately follows, Skip past the
         * key cell to the value cell, then skip to the start of the value's data.
         */
        value->data = (uint8_t *)WT_PAGE_REF_OFFSET(page, WT_KV_DECODE_KEY_CELL_OFFSET(v)) +
          WT_KV_DECODE_KEY_OFFSET(v) + WT_KV_DECODE_KEY_LEN(v) + WT_KV_DECODE_VALUE_OFFSET(v);
        value->size = WT_KV_DECODE_VALUE_LEN(v);
        return (true);
    }
    return (false);
}

/*
 * __wt_row_leaf_value_cell --
 *     Return the unpacked value for a row-store leaf page key.
 */
static inline void
__wt_row_leaf_value_cell(
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip, WT_CELL_UNPACK_KV *vpack)
{
    WT_CELL *kcell, *vcell;
    WT_CELL_UNPACK_KV unpack;
    WT_IKEY *ikey;
    uintptr_t v;

    /* The row-store key can change underfoot; explicitly take a copy. */
    v = (uintptr_t)WT_ROW_KEY_COPY(rip);

    kcell = vcell = NULL;
    switch (v & WT_KEY_FLAG_BITS) {
    case WT_CELL_FLAG:
        /* We have a direct reference the key's cell, step past it to the value's cell. */
        kcell = (WT_CELL *)WT_PAGE_REF_OFFSET(page, WT_CELL_DECODE_OFFSET(v));
        break;
    case WT_K_FLAG:
        /* We have an encoded on-page key, the value's cell follows the key's data. */
        vcell = (WT_CELL *)((uint8_t *)WT_PAGE_REF_OFFSET(page, WT_K_DECODE_KEY_CELL_OFFSET(v)) +
          WT_K_DECODE_KEY_OFFSET(v) + WT_K_DECODE_KEY_LEN(v));
        break;
    case WT_KV_FLAG:
        /* We have an encoded on-page key/value pair, the value's cell follows the key's data. */
        vcell = (WT_CELL *)((uint8_t *)WT_PAGE_REF_OFFSET(page, WT_KV_DECODE_KEY_CELL_OFFSET(v)) +
          WT_KV_DECODE_KEY_OFFSET(v) + WT_KV_DECODE_KEY_LEN(v));
        break;
    default:
        /* We have an instantiated key, the key cell's offset is included in the structure. */
        ikey = (WT_IKEY *)v;
        kcell =
          ikey->cell_offset == 0 ? NULL : (WT_CELL *)WT_PAGE_REF_OFFSET(page, ikey->cell_offset);
        break;
    }

    /* If we only have the key cell, unpack it and skip past it to the value cell. */
    if (vcell == NULL) {
        __wt_cell_unpack_kv(session, page->dsk, kcell, &unpack);
        vcell = (WT_CELL *)((uint8_t *)unpack.cell + __wt_cell_total_len(&unpack));
    }

    __wt_cell_unpack_kv(session, page->dsk, __wt_cell_leaf_value_parse(page, vcell), vpack);
}

/*
 * WT_ADDR_COPY --
 *	We have to lock the WT_REF to look at a WT_ADDR: a structure we can use to quickly get a
 * copy of the WT_REF address information.
 */
struct __wt_addr_copy {
    uint8_t type;

    uint8_t addr[WT_BTREE_MAX_ADDR_COOKIE];
    uint8_t size;

    WT_TIME_AGGREGATE ta;

    WT_PAGE_DELETED del; /* Fast-truncate page information */
    bool del_set;
};

/*
 * __wt_ref_addr_copy --
 *     Return a copy of the WT_REF address information.
 */
static inline bool
__wt_ref_addr_copy(WT_SESSION_IMPL *session, WT_REF *ref, WT_ADDR_COPY *copy)
{
    WT_ADDR *addr;
    WT_CELL_UNPACK_ADDR *unpack, _unpack;
    WT_PAGE *page;

    unpack = &_unpack;
    page = ref->home;
    copy->del_set = false;

    /*
     * To look at an on-page cell, we need to look at the parent page's disk image, and that can be
     * dangerous. The problem is if the parent page splits, deepening the tree. As part of that
     * process, the WT_REF WT_ADDRs pointing into the parent's disk image are copied into off-page
     * WT_ADDRs and swapped into place. The content of the two WT_ADDRs are identical, and we don't
     * care which version we get as long as we don't mix-and-match the two.
     */
    WT_ORDERED_READ(addr, (WT_ADDR *)ref->addr);

    /* If NULL, there is no information. */
    if (addr == NULL)
        return (false);

    /* If off-page, the pointer references a WT_ADDR structure. */
    if (__wt_off_page(page, addr)) {
        WT_TIME_AGGREGATE_COPY(&copy->ta, &addr->ta);
        copy->type = addr->type;
        memcpy(copy->addr, addr->addr, copy->size = addr->size);
        return (true);
    }

    /* If on-page, the pointer references a cell. */
    __wt_cell_unpack_addr(session, page->dsk, (WT_CELL *)addr, unpack);
    WT_TIME_AGGREGATE_COPY(&copy->ta, &unpack->ta);

    switch (unpack->raw) {
    case WT_CELL_ADDR_INT:
        copy->type = WT_ADDR_INT;
        break;
    case WT_CELL_ADDR_LEAF:
        copy->type = WT_ADDR_LEAF;
        break;
    case WT_CELL_ADDR_DEL:
        /* Copy out any fast-truncate information. */
        copy->del_set = true;
        if (F_ISSET(page->dsk, WT_PAGE_FT_UPDATE))
            copy->del = unpack->page_del;
        else {
            /* It's a legacy page; create default delete information. */
            copy->del.txnid = WT_TXN_NONE;
            copy->del.timestamp = copy->del.durable_timestamp = WT_TS_NONE;
            copy->del.prepare_state = 0;
            copy->del.previous_ref_state = WT_REF_DISK;
            copy->del.committed = true;
        }
        /* FALLTHROUGH */
    case WT_CELL_ADDR_LEAF_NO:
        copy->type = WT_ADDR_LEAF_NO;
        break;
    }
    memcpy(copy->addr, unpack->data, copy->size = (uint8_t)unpack->size);
    return (true);
}

/*
 * __wt_ref_block_free --
 *     Free the on-disk block for a reference and clear the address.
 */
static inline int
__wt_ref_block_free(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_ADDR_COPY addr;

    if (!__wt_ref_addr_copy(session, ref, &addr))
        return (0);

    WT_RET(__wt_btree_block_free(session, addr.addr, addr.size));

    /* Clear the address (so we don't free it twice). */
    __wt_ref_addr_free(session, ref);
    return (0);
}

/*
 * __wt_page_del_visible_all --
 *     Check if a truncate operation is visible to everyone and the data under it is obsolete.
 */
static inline bool
__wt_page_del_visible_all(WT_SESSION_IMPL *session, WT_PAGE_DELETED *page_del, bool hide_prepared)
{
    uint8_t prepare_state;

    /*
     * Like other visible_all checks, use the durable timestamp to avoid complications: there is
     * potentially a window where a prepared and committed transaction can be visible but not yet
     * durable, and in that window the changes under it are not obsolete yet.
     *
     * The hide_prepared argument causes prepared but not committed transactions to be treated as
     * invisible. (Apparently prepared and uncommitted transactions can be visible_all, but we need
     * to not see them in some cases; for example, prepared deletions can't exist on disk because
     * the on-disk format doesn't have space for the extra "I'm prepared" bit, so we avoid seeing
     * them in reconciliation. Similarly, we can't skip over a page just because a transaction has
     * deleted it and prepared; only committed transactions are suitable.)
     *
     * In all cases, the ref owning the page_deleted structure should be locked and its pre-lock
     * state should be WT_REF_DELETED. This prevents the page from being instantiated while we look
     * at it, and locks out other operations that might simultaneously discard the structure (either
     * after checking visibility, or because its transaction aborted).
     */

    /* If the page delete info is NULL, the deletion was previously found to be globally visible. */
    if (page_del == NULL)
        return (true);

    /* We discard page_del on transaction abort, so should never see an aborted one. */
    WT_ASSERT(session, page_del->txnid != WT_TXN_ABORTED);

    if (hide_prepared) {
        WT_ORDERED_READ(prepare_state, page_del->prepare_state);
        if (prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED)
            return (false);
    }

    return (__wt_txn_visible_all(session, page_del->txnid, page_del->durable_timestamp));
}

/*
 * __wt_page_del_visible --
 *     Return if a truncate operation is visible to the caller. The same considerations apply as in
 *     the visible_all version.
 */
static inline bool
__wt_page_del_visible(WT_SESSION_IMPL *session, WT_PAGE_DELETED *page_del, bool hide_prepared)
{
    uint8_t prepare_state;

    /* If the page delete info is NULL, the deletion was previously found to be globally visible. */
    if (page_del == NULL)
        return (true);

    /* We discard page_del on transaction abort, so should never see an aborted one. */
    WT_ASSERT(session, page_del->txnid != WT_TXN_ABORTED);

    if (hide_prepared) {
        WT_ORDERED_READ(prepare_state, page_del->prepare_state);
        if (prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED)
            return (false);
    }

    return (__wt_txn_visible(session, page_del->txnid, page_del->timestamp));
}

/*
 * __wt_page_del_committed --
 *     Return if a truncate operation is resolved. (Since truncations that abort are removed
 *     immediately, "resolved" and "committed" are equivalent here.) The caller should have already
 *     locked the ref and confirmed that the ref's previous state was WT_REF_DELETED. The page_del
 *     argument should be the ref's page_del member. This function should only be used for pages in
 *     WT_REF_DELETED state. For deleted pages that have been instantiated in memory, the update
 *     list in the page modify structure should be checked instead, as the page_del structure might
 *     have been discarded already. (The update list is non-null if the transaction is unresolved.)
 */
static inline bool
__wt_page_del_committed(WT_PAGE_DELETED *page_del)
{
    /*
     * There are two possible cases: either page_del is NULL (in which case the deletion is globally
     * visible and must have been committed) or it is not, in which case page_del->committed tells
     * us what we want to know.
     */

    if (page_del == NULL)
        return (true);

    return (page_del->committed);
}

/*
 * __wt_btree_syncing_by_other_session --
 *     Returns true if the session's current btree is being synced by another thread.
 */
static inline bool
__wt_btree_syncing_by_other_session(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;

    btree = S2BT(session);

    return (WT_BTREE_SYNCING(btree) && !WT_SESSION_BTREE_SYNC(session));
}

/*
 * __wt_leaf_page_can_split --
 *     Check whether a page can be split in memory.
 */
static inline bool
__wt_leaf_page_can_split(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_BTREE *btree;
    WT_INSERT *ins;
    WT_INSERT_HEAD *ins_head;
    size_t size;
    int count;

    btree = S2BT(session);

    /*
     * Checkpoints can't do in-memory splits in the tree they are walking: that can lead to
     * corruption when the parent internal page is updated.
     */
    if (WT_SESSION_BTREE_SYNC(session))
        return (false);

    /*
     * Only split a page once, otherwise workloads that update in the middle of the page could
     * continually split without benefit.
     */
    if (F_ISSET_ATOMIC_16(page, WT_PAGE_SPLIT_INSERT))
        return (false);

    /*
     * Check for pages with append-only workloads. A common application pattern is to have multiple
     * threads frantically appending to the tree. We want to reconcile and evict this page, but we'd
     * like to do it without making the appending threads wait. See if it's worth doing a split to
     * let the threads continue before doing eviction.
     *
     * Ignore anything other than large, dirty leaf pages. We depend on the page being dirty for
     * correctness (the page must be reconciled again before being evicted after the split,
     * information from a previous reconciliation will be wrong, so we can't evict immediately).
     */
    if (page->memory_footprint < btree->splitmempage)
        return (false);
    if (WT_PAGE_IS_INTERNAL(page))
        return (false);
    if (!__wt_page_is_modified(page))
        return (false);

    /*
     * There is no point doing an in-memory split unless there is a lot of data in the last skiplist
     * on the page. Split if there are enough items and the skiplist does not fit within a single
     * disk page.
     */
    ins_head = page->type == WT_PAGE_ROW_LEAF ?
      (page->entries == 0 ? WT_ROW_INSERT_SMALLEST(page) :
                            WT_ROW_INSERT_SLOT(page, page->entries - 1)) :
      WT_COL_APPEND(page);
    if (ins_head == NULL)
        return (false);

/*
 * In the extreme case, where the page is much larger than the maximum size, split as soon as there
 * are 5 items on the page.
 */
#define WT_MAX_SPLIT_COUNT 5
    if (page->memory_footprint > (size_t)btree->maxleafpage * 2) {
        for (count = 0, ins = ins_head->head[0]; ins != NULL; ins = ins->next[0]) {
            if (++count < WT_MAX_SPLIT_COUNT)
                continue;

            WT_STAT_CONN_DATA_INCR(session, cache_inmem_splittable);
            return (true);
        }

        return (false);
    }

/*
 * Rather than scanning the whole list, walk a higher level, which gives a sample of the items -- at
 * level 0 we have all the items, at level 1 we have 1/4 and at level 2 we have 1/16th. If we see
 * more than 30 items and more data than would fit in a disk page, split.
 */
#define WT_MIN_SPLIT_DEPTH 2
#define WT_MIN_SPLIT_COUNT 30
#define WT_MIN_SPLIT_MULTIPLIER 16 /* At level 2, we see 1/16th entries */

    for (count = 0, size = 0, ins = ins_head->head[WT_MIN_SPLIT_DEPTH]; ins != NULL;
         ins = ins->next[WT_MIN_SPLIT_DEPTH]) {
        count += WT_MIN_SPLIT_MULTIPLIER;
        size += WT_MIN_SPLIT_MULTIPLIER * (WT_INSERT_KEY_SIZE(ins) + WT_UPDATE_MEMSIZE(ins->upd));
        if (count > WT_MIN_SPLIT_COUNT && size > (size_t)btree->maxleafpage) {
            WT_STAT_CONN_DATA_INCR(session, cache_inmem_splittable);
            return (true);
        }
    }
    return (false);
}

/*
 * __wt_page_evict_retry --
 *     Avoid busy-spinning attempting to evict the same page all the time.
 */
static inline bool
__wt_page_evict_retry(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_PAGE_MODIFY *mod;
    WT_TXN_GLOBAL *txn_global;
    wt_timestamp_t pinned_ts;

    txn_global = &S2C(session)->txn_global;

    /*
     * If the page hasn't been through one round of update/restore, give it a try.
     */
    if ((mod = page->modify) == NULL || !FLD_ISSET(mod->restore_state, WT_PAGE_RS_RESTORED))
        return (true);

    /*
     * Retry if a reasonable amount of eviction time has passed, the choice of 5 eviction passes as
     * a reasonable amount of time is currently pretty arbitrary.
     */
    if (__wt_cache_aggressive(session) ||
      mod->last_evict_pass_gen + 5 < S2C(session)->cache->evict_pass_gen)
        return (true);

    /* Retry if the global transaction state has moved forward. */
    if (txn_global->current == txn_global->oldest_id ||
      mod->last_eviction_id != __wt_txn_oldest_id(session))
        return (true);

    /*
     * It is possible that we have not started using the timestamps just yet. So, check for the last
     * time we evicted only if there is a timestamp set.
     */
    if (mod->last_eviction_timestamp != WT_TS_NONE) {
        __wt_txn_pinned_timestamp(session, &pinned_ts);
        if (pinned_ts > mod->last_eviction_timestamp)
            return (true);
    }

    return (false);
}

/*
 * __wt_page_can_evict --
 *     Check whether a page can be evicted.
 */
static inline bool
__wt_page_can_evict(WT_SESSION_IMPL *session, WT_REF *ref, bool *inmem_splitp)
{
    WT_PAGE *page;
    WT_PAGE_MODIFY *mod;
    bool modified;

    if (inmem_splitp != NULL)
        *inmem_splitp = false;

    page = ref->page;
    mod = page->modify;

    /* Pages without modify structures can always be evicted, it's just discarding a disk image. */
    if (mod == NULL)
        return (true);

    /*
     * Check the fast-truncate information. Pages with an uncommitted truncate cannot be evicted.
     *
     * Because the page is in memory, we look at mod.inst_updates. If it's not NULL, that means the
     * truncate operation isn't committed.
     *
     * The list of updates in mod.inst_updates will be discarded when the transaction they belong to
     * is resolved.
     *
     * Note that we are not using __wt_page_del_committed here because (a) examining the page_del
     * structure requires locking the ref, and (b) once in memory the page_del structure only
     * remains until the next reconciliation, and nothing prevents that from occurring before the
     * transaction commits.
     */
    if (mod->inst_updates != NULL)
        return (false);

    /*
     * We can't split or evict multiblock row-store pages where the parent's key for the page is an
     * overflow item, because the split into the parent frees the backing blocks for no-longer-used
     * overflow keys, which will corrupt the checkpoint's block management. (This is only for
     * historical tables, reconciliation no longer writes overflow cookies on internal pages, no
     * matter the size of the key.)
     */
    if (__wt_btree_syncing_by_other_session(session) &&
      F_ISSET_ATOMIC_16(ref->home, WT_PAGE_INTL_OVERFLOW_KEYS))
        return (false);

    /*
     * Check for in-memory splits before other eviction tests. If the page should split in-memory,
     * return success immediately and skip more detailed eviction tests. We don't need further tests
     * since the page won't be written or discarded from the cache.
     */
    if (__wt_leaf_page_can_split(session, page)) {
        if (inmem_splitp != NULL)
            *inmem_splitp = true;
        return (true);
    }

    modified = __wt_page_is_modified(page);

    /*
     * If the file is being checkpointed, other threads can't evict dirty pages: if a page is
     * written and the previous version freed, that previous version might be referenced by an
     * internal page already written in the checkpoint, leaving the checkpoint inconsistent.
     */
    if (modified && __wt_btree_syncing_by_other_session(session)) {
        WT_STAT_CONN_DATA_INCR(session, cache_eviction_checkpoint);
        return (false);
    }

    /*
     * Check we are not evicting an accessible internal page with an active split generation.
     *
     * If a split created new internal pages, those newly created internal pages cannot be evicted
     * until all threads are known to have exited the original parent page's index, because evicting
     * an internal page discards its WT_REF array, and a thread traversing the original parent page
     * index might see a freed WT_REF.
     *
     * One special case where we know this is safe is if the handle is dead or locked exclusively,
     * that is, no readers can be looking at an old index.
     */
    if (F_ISSET(ref, WT_REF_FLAG_INTERNAL) &&
      !F_ISSET(session->dhandle, WT_DHANDLE_DEAD | WT_DHANDLE_EXCLUSIVE) &&
      __wt_gen_active(session, WT_GEN_SPLIT, page->pg_intl_split_gen))
        return (false);

    /* If the metadata page is clean but has modifications that appear too new to evict, skip it. */
    if (WT_IS_METADATA(S2BT(session)->dhandle) && !modified &&
      !__wt_txn_visible_all(session, mod->rec_max_txn, mod->rec_max_timestamp))
        return (false);

    return (true);
}

/*
 * __wt_page_release --
 *     Release a reference to a page.
 */
static inline int
__wt_page_release(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t flags)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    bool inmem_split;

    btree = S2BT(session);

    /*
     * Discard our hazard pointer. Ignore pages we don't have and the root page, which sticks in
     * memory, regardless.
     */
    if (ref == NULL || ref->page == NULL || __wt_ref_is_root(ref))
        return (0);

    /*
     * If hazard pointers aren't necessary for this file, we can't be evicting, we're done.
     */
    if (F_ISSET(btree, WT_BTREE_IN_MEMORY))
        return (0);

    /*
     * If the session is configured with the release_evict_pages debug option, we will attempt to
     * evict the pages when they are no longer needed.
     */
    if (F_ISSET(session, WT_SESSION_DEBUG_RELEASE_EVICT)) {
        WT_TRET_BUSY_OK(__wt_page_release_evict(session, ref, flags));
        return (0);
    }

    if (__wt_page_evict_soon_check(session, ref, &inmem_split)) {
        /*
         * If the operation has disabled eviction or splitting, or the session is preventing from
         * reconciling, then just queue the page for urgent eviction. Otherwise, attempt to release
         * and evict it.
         */
        if (LF_ISSET(WT_READ_NO_EVICT) ||
          (inmem_split ? LF_ISSET(WT_READ_NO_SPLIT) : F_ISSET(session, WT_SESSION_NO_RECONCILE)))
            WT_IGNORE_RET_BOOL(__wt_page_evict_urgent(session, ref));
        else {
            WT_RET_BUSY_OK(__wt_page_release_evict(session, ref, flags));
            return (0);
        }
    }

    return (__wt_hazard_clear(session, ref));
}

/*
 * __wt_skip_choose_depth --
 *     Randomly choose a depth for a skiplist insert.
 */
static inline u_int
__wt_skip_choose_depth(WT_SESSION_IMPL *session)
{
    u_int d;

    for (d = 1; d < WT_SKIP_MAXDEPTH && __wt_random(&session->rnd) < WT_SKIP_PROBABILITY; d++)
        ;
    return (d);
}

/*
 * __wt_btree_lsm_over_size --
 *     Return if the size of an in-memory tree with a single leaf page is over a specified maximum.
 *     If called on anything other than a simple tree with a single leaf page, returns true so our
 *     LSM caller will switch to a new tree.
 */
static inline bool
__wt_btree_lsm_over_size(WT_SESSION_IMPL *session, uint64_t maxsize)
{
    WT_BTREE *btree;
    WT_PAGE *child, *root;
    WT_PAGE_INDEX *pindex;
    WT_REF *first;

    btree = S2BT(session);
    root = btree->root.page;

    /* Check for a non-existent tree. */
    if (root == NULL)
        return (false);

    /* A tree that can be evicted always requires a switch. */
    if (btree->evict_disabled == 0)
        return (true);

    /* Check for a tree with a single leaf page. */
    WT_INTL_INDEX_GET(session, root, pindex);
    if (pindex->entries != 1) /* > 1 child page, switch */
        return (true);

    first = pindex->index[0];
    if (first->state != WT_REF_MEM) /* no child page, ignore */
        return (false);

    /*
     * We're reaching down into the page without a hazard pointer, but that's OK because we know
     * that no-eviction is set and so the page cannot disappear.
     */
    child = first->page;
    if (child->type != WT_PAGE_ROW_LEAF) /* not a single leaf page */
        return (true);

    return (child->memory_footprint > maxsize);
}

/*
 * __wt_split_descent_race --
 *     Return if we raced with an internal page split when descending the tree.
 */
static inline bool
__wt_split_descent_race(WT_SESSION_IMPL *session, WT_REF *ref, WT_PAGE_INDEX *saved_pindex)
{
    WT_PAGE_INDEX *pindex;

    /* No test when starting the descent (there's no home to check). */
    if (__wt_ref_is_root(ref))
        return (false);

    /*
     * A place to hang this comment...
     *
     * There's a page-split race when we walk the tree: if we're splitting
     * an internal page into its parent, we update the parent's page index
     * before updating the split page's page index, and it's not an atomic
     * update. A thread can read the parent page's original page index and
     * then read the split page's replacement index.
     *
     * For example, imagine a search descending the tree.
     *
     * Because internal page splits work by truncating the original page to
     * the initial part of the original page, the result of this race is we
     * will have a search key that points past the end of the current page.
     * This is only an issue when we search past the end of the page, if we
     * find a WT_REF in the page with the namespace we're searching for, we
     * don't care if the WT_REF moved or not while we were searching, we
     * have the correct page.
     *
     * For example, imagine an internal page with 3 child pages, with the
     * namespaces a-f, g-h and i-j; the first child page splits. The parent
     * starts out with the following page-index:
     *
     *	| ... | a | g | i | ... |
     *
     * which changes to this:
     *
     *	| ... | a | c | e | g | i | ... |
     *
     * The child starts out with the following page-index:
     *
     *	| a | b | c | d | e | f |
     *
     * which changes to this:
     *
     *	| a | b |
     *
     * The thread searches the original parent page index for the key "cat",
     * it couples to the "a" child page; if it uses the replacement child
     * page index, it will search past the end of the page and couple to the
     * "b" page, which is wrong.
     *
     * To detect the problem, we remember the parent page's page index used
     * to descend the tree. Whenever we search past the end of a page, we
     * check to see if the parent's page index has changed since our use of
     * it during descent. As the problem only appears if we read the split
     * page's replacement index, the parent page's index must already have
     * changed, ensuring we detect the problem.
     *
     * It's possible for the opposite race to happen (a thread could read
     * the parent page's replacement page index and then read the split
     * page's original index). This isn't a problem because internal splits
     * work by truncating the split page, so the split page search is for
     * content the split page retains after the split, and we ignore this
     * race.
     *
     * This code is a general purpose check for a descent race and we call
     * it in other cases, for example, a cursor traversing backwards through
     * the tree.
     *
     * Presumably we acquired a page index on the child page before calling
     * this code, don't re-order that acquisition with this check.
     */
    WT_BARRIER();
    WT_INTL_INDEX_GET(session, ref->home, pindex);
    return (pindex != saved_pindex);
}

/*
 * __wt_page_swap_func --
 *     Swap one page's hazard pointer for another one when hazard pointer coupling up/down the tree.
 */
static inline int
__wt_page_swap_func(WT_SESSION_IMPL *session, WT_REF *held, WT_REF *want, uint32_t flags
#ifdef HAVE_DIAGNOSTIC
  ,
  const char *func, int line
#endif
)
{
    WT_DECL_RET;
    bool acquired;

    /*
     * This function is here to simplify the error handling during hazard pointer coupling so we
     * never leave a hazard pointer dangling. The assumption is we're holding a hazard pointer on
     * "held", and want to acquire a hazard pointer on "want", releasing the hazard pointer on
     * "held" when we're done.
     *
     * When walking the tree, we sometimes swap to the same page. Fast-path that to avoid thinking
     * about error handling.
     */
    if (held == want)
        return (0);

    /* Get the wanted page. */
    ret = __wt_page_in_func(session, want, flags
#ifdef HAVE_DIAGNOSTIC
      ,
      func, line
#endif
    );

    /*
     * Expected failures: page not found or restart. Our callers list the errors they're expecting
     * to handle.
     */
    if (LF_ISSET(WT_READ_NOTFOUND_OK) && ret == WT_NOTFOUND)
        return (WT_NOTFOUND);
    if (LF_ISSET(WT_READ_RESTART_OK) && ret == WT_RESTART)
        return (WT_RESTART);

    /* Discard the original held page on either success or error. */
    acquired = ret == 0;
    WT_TRET(__wt_page_release(session, held, flags));

    /* Fast-path expected success. */
    if (ret == 0)
        return (0);

    /*
     * If there was an error at any point that our caller isn't prepared to handle, discard any page
     * we acquired.
     */
    if (acquired)
        WT_TRET(__wt_page_release(session, want, flags));

    /*
     * If we're returning an error, don't let it be one our caller expects to handle as returned by
     * page-in: the expectation includes the held page not having been released, and that's not the
     * case.
     */
    if (LF_ISSET(WT_READ_NOTFOUND_OK) && ret == WT_NOTFOUND)
        WT_RET_MSG(session, EINVAL, "page-release WT_NOTFOUND error mapped to EINVAL");
    if (LF_ISSET(WT_READ_RESTART_OK) && ret == WT_RESTART)
        WT_RET_MSG(session, EINVAL, "page-release WT_RESTART error mapped to EINVAL");

    return (ret);
}

/*
 * __wt_btcur_bounds_early_exit --
 *     Performs bound comparison to check if the key is within bounds, if not, increment the
 *     appropriate stat, early exit, and return WT_NOTFOUND.
 */
static inline int
__wt_btcur_bounds_early_exit(
  WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, bool next, bool *key_out_of_boundsp)
{
    uint64_t bound_flag;

    bound_flag = next ? WT_CURSTD_BOUND_UPPER : WT_CURSTD_BOUND_LOWER;

    if (!WT_CURSOR_BOUNDS_SET(&cbt->iface))
        return (0);
    if (!F_ISSET((&cbt->iface), bound_flag))
        return (0);

    WT_RET(__wt_compare_bounds(
      session, &cbt->iface, &cbt->iface.key, cbt->recno, next, key_out_of_boundsp));

    if (*key_out_of_boundsp)
        return (WT_NOTFOUND);

    return (0);
}

/*
 * __wt_btcur_skip_page --
 *     Return if the cursor is pointing to a page with deleted records and can be skipped for cursor
 *     traversal.
 */
static inline int
__wt_btcur_skip_page(
  WT_SESSION_IMPL *session, WT_REF *ref, void *context, bool visible_all, bool *skipp)
{
    WT_ADDR_COPY addr;
    WT_BTREE *btree;
    uint8_t previous_state;

    WT_UNUSED(context);
    WT_UNUSED(visible_all);

    *skipp = false; /* Default to reading */

    btree = S2BT(session);

    /* Don't skip pages in FLCS trees; deleted records need to read back as 0. */
    if (btree->type == BTREE_COL_FIX)
        return (0);

    /*
     * Determine if all records on the page have been deleted and all the tombstones are visible to
     * our transaction. If so, we can avoid reading the records on the page and move to the next
     * page.
     *
     * Skip this test on an internal page, as we rely on reconciliation to mark the internal page
     * dirty. There could be a period of time when the internal page is marked clean but the leaf
     * page is dirty and has newer data than let on by the internal page's aggregated information.
     */
    if (F_ISSET(ref, WT_REF_FLAG_INTERNAL))
        return (0);

    /*
     * We are making these decisions while holding a lock for the page as checkpoint or eviction can
     * make changes to the data structures (i.e., aggregate timestamps) we are reading.
     */
    WT_REF_LOCK(session, ref, &previous_state);

    /*
     * Check the fast-truncate information; there are 3 cases:
     *
     * (1) The page is in the WT_REF_DELETED state and page_del is NULL. The page is deleted. This
     *     case is folded into the next because __wt_page_del_visible handles it.
     * (2) The page is in the WT_REF_DELETED state and page_del is not NULL. The page is deleted
     *     if the truncate operation is visible. Look at page_del; we could use the info from the
     *     address cell below too, but that's slower.
     * (3) The page is in memory and has been instantiated. The delete info from the address cell
     *     will serve for readonly/unmodified pages, and for modified pages we can't skip the page.
     *     (This case is checked further below.)
     *
     * In all cases, make use of the option to __wt_page_del_visible to hide prepared transactions,
     * as we shouldn't skip pages where the deletion is prepared but not committed.
     */
    if (previous_state == WT_REF_DELETED && __wt_page_del_visible(session, ref->page_del, true)) {
        *skipp = true;
        goto unlock;
    }

    /*
     * Look at the disk address, if it exists, and if the page is unmodified. We must skip this test
     * if the page has been modified since it was reconciled, since neither the delete information
     * nor the timestamp information is necessarily up to date.
     */
    if ((previous_state == WT_REF_DISK ||
          (previous_state == WT_REF_MEM && !__wt_page_is_modified(ref->page))) &&
      __wt_ref_addr_copy(session, ref, &addr)) {
        /* If there's delete information in the disk address, we can use it. */
        if (addr.del_set && __wt_page_del_visible(session, &addr.del, true)) {
            *skipp = true;
            goto unlock;
        }

        /*
         * Otherwise, check the timestamp information. We base this decision on the aggregate stop
         * point added to the page during the last reconciliation.
         */
        if (addr.ta.newest_stop_txn != WT_TXN_MAX && addr.ta.newest_stop_ts != WT_TS_MAX &&
          __wt_txn_visible(session, addr.ta.newest_stop_txn, addr.ta.newest_stop_ts)) {
            *skipp = true;
            goto unlock;
        }
    }

unlock:
    WT_REF_UNLOCK(ref, previous_state);
    return (0);
}
