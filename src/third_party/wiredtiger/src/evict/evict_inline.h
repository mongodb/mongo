/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * __wt_evict_aggressive --
 *     Indicate if the eviction is operating in aggressive mode.
 */
static WT_INLINE bool
__wt_evict_aggressive(WT_SESSION_IMPL *session)
{
    return (
      __wt_atomic_load32(&S2C(session)->evict->evict_aggressive_score) >= WT_EVICT_SCORE_CUTOFF);
}

/*
 * __wt_evict_cache_stuck --
 *     Indicate if the cache is stuck (i.e. eviction not making progress).
 */
static WT_INLINE bool
__wt_evict_cache_stuck(WT_SESSION_IMPL *session)
{
    WT_EVICT *evict;
    uint32_t tmp_evict_aggressive_score;

    evict = S2C(session)->evict;
    tmp_evict_aggressive_score = __wt_atomic_load32(&evict->evict_aggressive_score);
    WT_ASSERT(session, tmp_evict_aggressive_score <= WT_EVICT_SCORE_MAX);
    return (
      tmp_evict_aggressive_score == WT_EVICT_SCORE_MAX && F_ISSET(evict, WT_EVICT_CACHE_HARD));
}

/*
 * __evict_read_gen --
 *     Get the current read generation number.
 */
static WT_INLINE uint64_t
__evict_read_gen(WT_SESSION_IMPL *session)
{
    return (__wt_atomic_load64(&S2C(session)->evict->read_gen));
}

/*
 * __wti_evict_read_gen_bump --
 *     Update the page's read generation.
 */
static WT_INLINE void
__wti_evict_read_gen_bump(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    /* Ignore pages set for forcible eviction. */
    if (__wt_atomic_load64(&page->read_gen) == WT_READGEN_EVICT_SOON)
        return;

    /* Ignore pages already in the future. */
    if (__wt_atomic_load64(&page->read_gen) > __evict_read_gen(session))
        return;

    /*
     * We set read-generations in the future (where "the future" is measured by increments of the
     * global read generation). The reason is because when acquiring a new hazard pointer for a
     * page, we can check its read generation, and if the read generation isn't less than the
     * current global generation, we don't bother updating the page. In other words, the goal is to
     * avoid some number of updates immediately after each update we have to make.
     */
    __wt_atomic_store64(&page->read_gen, __evict_read_gen(session) + WT_READGEN_STEP);
}

/*
 * __wti_evict_read_gen_new --
 *     Get the read generation for a new page in memory.
 */
static WT_INLINE void
__wti_evict_read_gen_new(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    __wt_atomic_store64(
      &page->read_gen, (__evict_read_gen(session) + S2C(session)->evict->read_gen_oldest) / 2);
}

/*
 * __wti_evict_readgen_is_soon_or_wont_need --
 *     Return whether a read generation value makes a page eligible for forced eviction. Read
 *     generations reserve a range of low numbers for special meanings and currently - with the
 *     exception of the generation not being set - these indicate the page may be evicted
 *     forcefully.
 */
static WT_INLINE bool
__wti_evict_readgen_is_soon_or_wont_need(uint64_t *readgen)
{
    uint64_t gen;

    WT_READ_ONCE(gen, *readgen);
    return (gen != WT_READGEN_NOTSET && gen < WT_READGEN_START_VALUE);
}

/*
 * __wt_evict_page_is_soon_or_wont_need --
 *     Return whether the page is eligible for forced eviction.
 */
static WT_INLINE bool
__wt_evict_page_is_soon_or_wont_need(WT_PAGE *page)
{
    return (__wti_evict_readgen_is_soon_or_wont_need(&page->read_gen));
}

/*
 * __wt_evict_page_is_soon --
 *     Return whether the page is to be evicted as soon as possible.
 */
static WT_INLINE bool
__wt_evict_page_is_soon(WT_PAGE *page)
{
    return (__wt_atomic_load64(&page->read_gen) == WT_READGEN_EVICT_SOON);
}

/*
 * __wt_evict_page_soon --
 *     Set a page to be evicted as soon as possible.
 */
static WT_INLINE void
__wt_evict_page_soon(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_UNUSED(session);

    __wt_atomic_store64(&ref->page->read_gen, WT_READGEN_EVICT_SOON);
}

/*
 * __wt_evict_page_first_dirty --
 *     Tell eviction when a page transitions from clean to dirty. The eviction mechanism will then
 *     update the page's eviction state as needed.
 */
static WT_INLINE void
__wt_evict_page_first_dirty(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    /*
     * In the event we dirty a page which is flagged as wont need, we update its read generation to
     * avoid evicting a dirty page prematurely.
     */
    if (__wt_atomic_load64(&page->read_gen) == WT_READGEN_WONT_NEED)
        __wti_evict_read_gen_new(session, page);
}

/*
 * __wt_evict_touch_page --
 *     Tell eviction when we touch a page so it can update its eviction state for that page. The
 *     caller may set flags indicating that it doesn't expect to need the page again or that it's an
 *     internal operation which doesn't change eviction state. The latter is used by operations such
 *     as compact, and eviction, itself, so internal operations don't update page's eviction state.
 */
static WT_INLINE void
__wt_evict_touch_page(WT_SESSION_IMPL *session, WT_PAGE *page, bool internal_only, bool wont_need)
{
    /* Is this the first use of the page? */
    if (__wt_atomic_load64(&page->read_gen) == WT_READGEN_NOTSET) {
        if (wont_need)
            __wt_atomic_store64(&page->read_gen, WT_READGEN_WONT_NEED);
        else
            __wti_evict_read_gen_new(session, page);
    } else if (!internal_only)
        __wti_evict_read_gen_bump(session, page);
}

/*
 * __wt_evict_page_init --
 *     Initialize page's eviction state for a newly created page.
 */
static WT_INLINE void
__wt_evict_page_init(WT_PAGE *page)
{
    __wt_atomic_store64(&page->read_gen, WT_READGEN_NOTSET);
}

/*
 * __wt_evict_inherit_page_state --
 *     When creating a new page from an existing page, for example during split, initialize the read
 *     generation on the new page using the read generation of the original page, unless this was a
 *     forced eviction, in which case we leave the new page with the default initialization.
 */
static WT_INLINE void
__wt_evict_inherit_page_state(WT_PAGE *orig_page, WT_PAGE *new_page)
{
    uint64_t orig_read_gen;

    WT_READ_ONCE(orig_read_gen, orig_page->read_gen);

    if (!__wti_evict_readgen_is_soon_or_wont_need(&orig_read_gen))
        __wt_atomic_store64(&new_page->read_gen, orig_read_gen);
}

/*
 * __wt_evict_page_cache_bytes_decr --
 *     Decrement the cache, btree, and page byte count in-memory to reflect eviction.
 */
static WT_INLINE void
__wt_evict_page_cache_bytes_decr(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_BTREE *btree;
    WT_CACHE *cache;
    WT_PAGE_MODIFY *modify;

    btree = S2BT(session);
    cache = S2C(session)->cache;
    modify = page->modify;

    /* Update the bytes in-memory to reflect the eviction. */
    __wt_cache_decr_check_uint64(session, &btree->bytes_inmem,
      __wt_atomic_loadsize(&page->memory_footprint), "WT_BTREE.bytes_inmem");
    __wt_cache_decr_check_uint64(session, &cache->bytes_inmem,
      __wt_atomic_loadsize(&page->memory_footprint), "WT_CACHE.bytes_inmem");

    /* Update the bytes_internal value to reflect the eviction */
    if (WT_PAGE_IS_INTERNAL(page)) {
        __wt_cache_decr_check_uint64(session, &btree->bytes_internal,
          __wt_atomic_loadsize(&page->memory_footprint), "WT_BTREE.bytes_internal");
        __wt_cache_decr_check_uint64(session, &cache->bytes_internal,
          __wt_atomic_loadsize(&page->memory_footprint), "WT_CACHE.bytes_internal");
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
    (void)__wt_atomic_add64(&cache->bytes_evict, __wt_atomic_loadsize(&page->memory_footprint));
    (void)__wt_atomic_addv64(&cache->pages_evicted, 1);

    /*
     * Track if eviction makes progress. This is used in various places to determine whether
     * eviction is stuck.
     */
    if (!F_ISSET_ATOMIC_16(page, WT_PAGE_EVICT_NO_PROGRESS))
        (void)__wt_atomic_addv64(&S2C(session)->evict->eviction_progress, 1);
}

/*
 * __wt_evict_clean_pressure --
 *     Return true if clean cache is stressed and will soon require application threads to evict
 *     content.
 */
static WT_INLINE bool
__wt_evict_clean_pressure(WT_SESSION_IMPL *session)
{
    WT_EVICT *evict;
    double pct_full;

    evict = S2C(session)->evict;
    pct_full = 0;

    /* Eviction should be done if we hit the eviction clean trigger or come close to hitting it. */
    if (__wt_evict_clean_needed(session, &pct_full))
        return (true);
    if (pct_full > evict->eviction_target &&
      pct_full >= WT_EVICT_PRESSURE_THRESHOLD * evict->eviction_trigger)
        return (true);
    return (false);
}

/*
 * __wt_evict_clean_needed --
 *     Return if an application thread should do eviction due to the total volume of data in cache.
 */
static WT_INLINE bool
__wt_evict_clean_needed(WT_SESSION_IMPL *session, double *pct_fullp)
{
    uint64_t bytes_inuse, bytes_max;

    /*
     * Avoid division by zero if the cache size has not yet been set in a shared cache.
     */
    bytes_max = S2C(session)->cache_size + 1;
    bytes_inuse = __wt_cache_bytes_inuse(S2C(session)->cache);

    if (pct_fullp != NULL)
        *pct_fullp = ((100.0 * bytes_inuse) / bytes_max);

    return (bytes_inuse > (S2C(session)->evict->eviction_trigger * bytes_max) / 100);
}

/*
 * __wti_evict_dirty_target --
 *     Return the effective dirty target (including checkpoint scrubbing).
 */
static WT_INLINE double
__wti_evict_dirty_target(WT_EVICT *evict)
{
    double dirty_target, scrub_target;

    dirty_target = __wt_read_shared_double(&evict->eviction_dirty_target);
    scrub_target = __wt_read_shared_double(&evict->eviction_scrub_target);

    return (scrub_target > 0 && scrub_target < dirty_target ? scrub_target : dirty_target);
}

/*
 * __wt_evict_dirty_needed --
 *     Return if an application thread should do eviction due to the total volume of dirty data in
 *     cache.
 */
static WT_INLINE bool
__wt_evict_dirty_needed(WT_SESSION_IMPL *session, double *pct_fullp)
{
    uint64_t bytes_dirty, bytes_max;

    /*
     * Avoid division by zero if the cache size has not yet been set in a shared cache.
     */
    bytes_dirty = __wt_cache_dirty_leaf_inuse(S2C(session)->cache);
    bytes_max = S2C(session)->cache_size + 1;

    if (pct_fullp != NULL)
        *pct_fullp = (100.0 * bytes_dirty) / bytes_max;

    return (
      bytes_dirty > (uint64_t)(S2C(session)->evict->eviction_dirty_trigger * bytes_max) / 100);
}

/*
 * __wti_evict_updates_needed --
 *     Return if an application thread should do eviction due to the total volume of updates in
 *     cache.
 */
static WT_INLINE bool
__wti_evict_updates_needed(WT_SESSION_IMPL *session, double *pct_fullp)
{
    uint64_t bytes_max, bytes_updates;

    /*
     * Avoid division by zero if the cache size has not yet been set in a shared cache.
     */
    bytes_max = S2C(session)->cache_size + 1;
    bytes_updates = __wt_cache_bytes_updates(S2C(session)->cache);

    if (pct_fullp != NULL)
        *pct_fullp = (100.0 * bytes_updates) / bytes_max;

    return (
      bytes_updates > (uint64_t)(S2C(session)->evict->eviction_updates_trigger * bytes_max) / 100);
}

/*
 * __wt_evict_needed --
 *     Return if an application thread should do eviction, and the cache full percentage as a
 *     side-effect.
 */
static WT_INLINE bool
__wt_evict_needed(WT_SESSION_IMPL *session, bool busy, bool readonly, double *pct_fullp)
{
    WT_EVICT *evict;
    double pct_dirty, pct_full, pct_updates;
    bool clean_needed, dirty_needed, updates_needed;

    evict = S2C(session)->evict;

    /*
     * If the connection is closing we do not need eviction from an application thread. The eviction
     * subsystem is already closed.
     */
    if (F_ISSET(S2C(session), WT_CONN_CLOSING))
        return (false);

    clean_needed = __wt_evict_clean_needed(session, &pct_full);
    if (readonly) {
        dirty_needed = updates_needed = false;
        pct_dirty = pct_updates = 0.0;
    } else {
        dirty_needed = __wt_evict_dirty_needed(session, &pct_dirty);
        updates_needed = __wti_evict_updates_needed(session, &pct_updates);
    }

    /*
     * Calculate the cache full percentage; anything over the trigger means we involve the
     * application thread.
     */
    if (pct_fullp != NULL)
        *pct_fullp = WT_MAX(0.0,
          100.0 -
            WT_MIN(
              WT_MIN(evict->eviction_trigger - pct_full, evict->eviction_dirty_trigger - pct_dirty),
              evict->eviction_updates_trigger - pct_updates));

    /*
     * Only check the dirty trigger when the session is not busy.
     *
     * In other words, once we are pinning resources, try to finish the operation as quickly as
     * possible without exceeding the cache size. The next transaction in this session will not be
     * able to start until the cache is under the limit.
     */
    return (clean_needed || updates_needed || (!busy && dirty_needed));
}

/*
 * __wt_evict_favor_clearing_dirty_cache --
 *     !!! This function should only be called when closing WiredTiger. It aggressively adjusts
 *     eviction settings to remove dirty bytes from the cache. Use with caution as this will
 *     significantly impact eviction behavior.
 */
static WT_INLINE void
__wt_evict_favor_clearing_dirty_cache(WT_SESSION_IMPL *session)
{
    WT_EVICT *evict;

    evict = S2C(session)->evict;

    /*
     * Ramp the eviction dirty target down to encourage eviction threads to clear dirty content out
     * of cache.
     */
    __wt_set_shared_double(&evict->eviction_dirty_trigger, 1.0);
    __wt_set_shared_double(&evict->eviction_dirty_target, 0.1);
}

/*
 * __wti_evict_hs_dirty --
 *     Return if a major portion of the cache is dirty due to history store content.
 */
static WT_INLINE bool
__wti_evict_hs_dirty(WT_SESSION_IMPL *session)
{
    WT_CACHE *cache;
    WT_CONNECTION_IMPL *conn;
    uint64_t bytes_max;
    conn = S2C(session);
    cache = conn->cache;
    bytes_max = conn->cache_size;

    return (__wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&cache->bytes_hs_dirty)) >=
      ((uint64_t)(conn->evict->eviction_dirty_trigger * bytes_max) / 100));
}

/*
 * __wt_evict_app_assist_worker_check --
 *     Evict pages if the cache crosses eviction trigger thresholds.
 */
static WT_INLINE int
__wt_evict_app_assist_worker_check(
  WT_SESSION_IMPL *session, bool busy, bool readonly, bool *didworkp)
{
    WT_BTREE *btree;
    WT_TXN_GLOBAL *txn_global;
    WT_TXN_SHARED *txn_shared;
    double pct_full;

    if (didworkp != NULL)
        *didworkp = false;

    /* Eviction causes reconciliation. So don't evict if we can't reconcile */
    if (F_ISSET(session, WT_SESSION_NO_RECONCILE))
        return (0);

    /* If the transaction is prepared don't evict. */
    if (F_ISSET(session->txn, WT_TXN_PREPARE))
        return (0);

    /*
     * If the transaction is a checkpoint cursor transaction, don't try to evict. Because eviction
     * keeps the current transaction snapshot, and the snapshot in a checkpoint cursor transaction
     * can be (and likely is) very old, we won't be able to see anything current to evict and won't
     * be able to accomplish anything useful.
     */
    if (F_ISSET(session->txn, WT_TXN_IS_CHECKPOINT))
        return (0);

    /*
     * If the current transaction is keeping the oldest ID pinned, it is in the middle of an
     * operation. This may prevent the oldest ID from moving forward, leading to deadlock, so only
     * evict what we can. Otherwise, we are at a transaction boundary and we can work harder to make
     * sure there is free space in the cache.
     */
    txn_global = &S2C(session)->txn_global;
    txn_shared = WT_SESSION_TXN_SHARED(session);
    busy = busy || __wt_atomic_loadv64(&txn_shared->id) != WT_TXN_NONE ||
      session->hazards.num_active > 0 ||
      (__wt_atomic_loadv64(&txn_shared->pinned_id) != WT_TXN_NONE &&
        __wt_atomic_loadv64(&txn_global->current) != __wt_atomic_loadv64(&txn_global->oldest_id));

    /*
     * LSM sets the "ignore cache size" flag when holding the LSM tree lock, in that case, or when
     * holding the handle list, schema or table locks (which can block checkpoints and eviction),
     * don't block the thread for eviction.
     */
    if (F_ISSET(session, WT_SESSION_IGNORE_CACHE_SIZE) ||
      FLD_ISSET(session->lock_flags,
        WT_SESSION_LOCKED_HANDLE_LIST | WT_SESSION_LOCKED_SCHEMA | WT_SESSION_LOCKED_TABLE))
        return (0);

    /* In memory configurations don't block when the cache is full. */
    if (F_ISSET(S2C(session), WT_CONN_IN_MEMORY))
        return (0);

    /*
     * Threads operating on cache-resident trees are ignored because they're not contributing to the
     * problem. We also don't block while reading metadata because we're likely to be holding some
     * other resources that could block checkpoints or eviction.
     */
    btree = S2BT_SAFE(session);
    if (btree != NULL && (F_ISSET(btree, WT_BTREE_IN_MEMORY) || WT_IS_METADATA(session->dhandle)))
        return (0);

    /* Check if eviction is needed. */
    if (!__wt_evict_needed(session, busy, readonly, &pct_full))
        return (0);

    /*
     * Some callers (those waiting for slow operations), will sleep if there was no cache work to
     * do. After this point, let them skip the sleep.
     */
    if (didworkp != NULL)
        *didworkp = true;

    return (__wti_evict_app_assist_worker(session, busy, readonly, pct_full));
}
