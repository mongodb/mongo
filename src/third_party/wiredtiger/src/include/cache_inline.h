/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * __wt_cache_aggressive --
 *     Indicate if the cache is operating in aggressive mode.
 */
static WT_INLINE bool
__wt_cache_aggressive(WT_SESSION_IMPL *session)
{
    return (
      __wt_atomic_load32(&S2C(session)->cache->evict_aggressive_score) >= WT_EVICT_SCORE_CUTOFF);
}

/*
 * __wt_cache_read_gen --
 *     Get the current read generation number.
 */
static WT_INLINE uint64_t
__wt_cache_read_gen(WT_SESSION_IMPL *session)
{
    return (__wt_atomic_load64(&S2C(session)->cache->read_gen));
}

/*
 * __wt_cache_read_gen_incr --
 *     Increment the current read generation number.
 */
static WT_INLINE void
__wt_cache_read_gen_incr(WT_SESSION_IMPL *session)
{
    (void)__wt_atomic_add64(&S2C(session)->cache->read_gen, 1);
}

/*
 * __wt_cache_read_gen_bump --
 *     Update the page's read generation.
 */
static WT_INLINE void
__wt_cache_read_gen_bump(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    /* Ignore pages set for forcible eviction. */
    if (__wt_atomic_load64(&page->read_gen) == WT_READGEN_OLDEST)
        return;

    /* Ignore pages already in the future. */
    if (__wt_atomic_load64(&page->read_gen) > __wt_cache_read_gen(session))
        return;

    /*
     * We set read-generations in the future (where "the future" is measured by increments of the
     * global read generation). The reason is because when acquiring a new hazard pointer for a
     * page, we can check its read generation, and if the read generation isn't less than the
     * current global generation, we don't bother updating the page. In other words, the goal is to
     * avoid some number of updates immediately after each update we have to make.
     */
    __wt_atomic_store64(&page->read_gen, __wt_cache_read_gen(session) + WT_READGEN_STEP);
}

/*
 * __wt_cache_read_gen_new --
 *     Get the read generation for a new page in memory.
 */
static WT_INLINE void
__wt_cache_read_gen_new(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_CACHE *cache;

    cache = S2C(session)->cache;
    __wt_atomic_store64(
      &page->read_gen, (__wt_cache_read_gen(session) + cache->read_gen_oldest) / 2);
}

/*
 * __wt_cache_stuck --
 *     Indicate if the cache is stuck (i.e., not making progress).
 */
static WT_INLINE bool
__wt_cache_stuck(WT_SESSION_IMPL *session)
{
    WT_CACHE *cache;
    uint32_t tmp_evict_aggressive_score;

    cache = S2C(session)->cache;
    tmp_evict_aggressive_score = __wt_atomic_load32(&cache->evict_aggressive_score);
    WT_ASSERT(session, tmp_evict_aggressive_score <= WT_EVICT_SCORE_MAX);
    return (
      tmp_evict_aggressive_score == WT_EVICT_SCORE_MAX && F_ISSET(cache, WT_CACHE_EVICT_HARD));
}

/*
 * __wt_page_evict_soon --
 *     Set a page to be evicted as soon as possible.
 */
static WT_INLINE void
__wt_page_evict_soon(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_UNUSED(session);

    __wt_atomic_store64(&ref->page->read_gen, WT_READGEN_OLDEST);
}

/*
 * __wt_page_dirty_and_evict_soon --
 *     Mark a page dirty and set it to be evicted as soon as possible.
 */
static WT_INLINE int
__wt_page_dirty_and_evict_soon(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_RET(__wt_page_modify_init(session, ref->page));
    __wt_page_modify_set(session, ref->page);
    __wt_page_evict_soon(session, ref);

    return (0);
}

/*
 * __wt_cache_pages_inuse --
 *     Return the number of pages in use.
 */
static WT_INLINE uint64_t
__wt_cache_pages_inuse(WT_CACHE *cache)
{
    return (cache->pages_inmem - cache->pages_evicted);
}

/*
 * __wt_cache_bytes_plus_overhead --
 *     Apply the cache overhead to a size in bytes.
 */
static WT_INLINE uint64_t
__wt_cache_bytes_plus_overhead(WT_CACHE *cache, uint64_t sz)
{
    if (cache->overhead_pct != 0)
        sz += (sz * (uint64_t)cache->overhead_pct) / 100;

    return (sz);
}

/*
 * __wt_cache_bytes_inuse --
 *     Return the number of bytes in use.
 */
static WT_INLINE uint64_t
__wt_cache_bytes_inuse(WT_CACHE *cache)
{
    return (__wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&cache->bytes_inmem)));
}

/*
 * __wt_cache_dirty_inuse --
 *     Return the number of dirty bytes in use.
 */
static WT_INLINE uint64_t
__wt_cache_dirty_inuse(WT_CACHE *cache)
{
    uint64_t dirty_inuse;
    dirty_inuse =
      __wt_atomic_load64(&cache->bytes_dirty_intl) + __wt_atomic_load64(&cache->bytes_dirty_leaf);
    return (__wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&dirty_inuse)));
}

/*
 * __wt_cache_dirty_intl_inuse --
 *     Return the number of dirty bytes in use by internal pages.
 */
static WT_INLINE uint64_t
__wt_cache_dirty_intl_inuse(WT_CACHE *cache)
{
    return (__wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&cache->bytes_dirty_intl)));
}

/*
 * __wt_cache_dirty_leaf_inuse --
 *     Return the number of dirty bytes in use by leaf pages.
 */
static WT_INLINE uint64_t
__wt_cache_dirty_leaf_inuse(WT_CACHE *cache)
{
    return (__wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&cache->bytes_dirty_leaf)));
}

/*
 * __wt_cache_bytes_updates --
 *     Return the number of bytes in use for updates.
 */
static WT_INLINE uint64_t
__wt_cache_bytes_updates(WT_CACHE *cache)
{
    return (__wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&cache->bytes_updates)));
}

/*
 * __wt_cache_bytes_image --
 *     Return the number of page image bytes in use.
 */
static WT_INLINE uint64_t
__wt_cache_bytes_image(WT_CACHE *cache)
{
    uint64_t bytes_image;
    bytes_image =
      __wt_atomic_load64(&cache->bytes_image_intl) + __wt_atomic_load64(&cache->bytes_image_leaf);
    return (__wt_cache_bytes_plus_overhead(cache, bytes_image));
}

/*
 * __wt_cache_bytes_other --
 *     Return the number of bytes in use not for page images.
 */
static WT_INLINE uint64_t
__wt_cache_bytes_other(WT_CACHE *cache)
{
    uint64_t bytes_other, bytes_inmem, bytes_image_intl, bytes_image_leaf;

    bytes_inmem = __wt_atomic_load64(&cache->bytes_inmem);
    bytes_image_intl = __wt_atomic_load64(&cache->bytes_image_intl);
    bytes_image_leaf = __wt_atomic_load64(&cache->bytes_image_leaf);
    /*
     * Reads can race with changes to the values, so check that the calculation doesn't go negative.
     */
    bytes_other = __wt_safe_sub(bytes_inmem, bytes_image_intl + bytes_image_leaf);
    return (__wt_cache_bytes_plus_overhead(cache, bytes_other));
}

/*
 * __wt_session_can_wait --
 *     Return if a session available for a potentially slow operation.
 */
static WT_INLINE bool
__wt_session_can_wait(WT_SESSION_IMPL *session)
{
    /*
     * Return if a session available for a potentially slow operation; for example, used by the
     * block manager in the case of flushing the system cache.
     */
    if (!F_ISSET(session, WT_SESSION_CAN_WAIT))
        return (false);

    /*
     * LSM sets the "ignore cache size" flag when holding the LSM tree lock, in that case, or when
     * holding the schema lock, we don't want this thread to block for eviction.
     */
    return (!(F_ISSET(session, WT_SESSION_IGNORE_CACHE_SIZE) ||
      FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SCHEMA)));
}

/*
 * __wt_eviction_clean_pressure --
 *     Return true if clean cache is stressed and will soon require application threads to evict
 *     content.
 */
static WT_INLINE bool
__wt_eviction_clean_pressure(WT_SESSION_IMPL *session)
{
    WT_CACHE *cache;
    double pct_full;

    cache = S2C(session)->cache;
    pct_full = 0;

    /* Eviction should be done if we hit the eviction clean trigger or come close to hitting it. */
    if (__wt_eviction_clean_needed(session, &pct_full))
        return (true);
    if (pct_full > cache->eviction_target &&
      pct_full >= WT_EVICT_PRESSURE_THRESHOLD * cache->eviction_trigger)
        return (true);
    return (false);
}

/*
 * __wt_eviction_clean_needed --
 *     Return if an application thread should do eviction due to the total volume of data in cache.
 */
static WT_INLINE bool
__wt_eviction_clean_needed(WT_SESSION_IMPL *session, double *pct_fullp)
{
    WT_CACHE *cache;
    uint64_t bytes_inuse, bytes_max;

    cache = S2C(session)->cache;

    /*
     * Avoid division by zero if the cache size has not yet been set in a shared cache.
     */
    bytes_max = S2C(session)->cache_size + 1;
    bytes_inuse = __wt_cache_bytes_inuse(cache);

    if (pct_fullp != NULL)
        *pct_fullp = ((100.0 * bytes_inuse) / bytes_max);

    return (bytes_inuse > (cache->eviction_trigger * bytes_max) / 100);
}

/*
 * __wt_eviction_dirty_target --
 *     Return the effective dirty target (including checkpoint scrubbing).
 */
static WT_INLINE double
__wt_eviction_dirty_target(WT_CACHE *cache)
{
    double dirty_target, scrub_target;

    dirty_target = cache->eviction_dirty_target;
    scrub_target = cache->eviction_scrub_target;

    return (scrub_target > 0 && scrub_target < dirty_target ? scrub_target : dirty_target);
}

/*
 * __wt_eviction_dirty_needed --
 *     Return if an application thread should do eviction due to the total volume of dirty data in
 *     cache.
 */
static WT_INLINE bool
__wt_eviction_dirty_needed(WT_SESSION_IMPL *session, double *pct_fullp)
{
    WT_CACHE *cache;
    uint64_t bytes_dirty, bytes_max;

    cache = S2C(session)->cache;

    /*
     * Avoid division by zero if the cache size has not yet been set in a shared cache.
     */
    bytes_dirty = __wt_cache_dirty_leaf_inuse(cache);
    bytes_max = S2C(session)->cache_size + 1;

    if (pct_fullp != NULL)
        *pct_fullp = (100.0 * bytes_dirty) / bytes_max;

    return (bytes_dirty > (uint64_t)(cache->eviction_dirty_trigger * bytes_max) / 100);
}

/*
 * __wt_eviction_updates_needed --
 *     Return if an application thread should do eviction due to the total volume of updates in
 *     cache.
 */
static WT_INLINE bool
__wt_eviction_updates_needed(WT_SESSION_IMPL *session, double *pct_fullp)
{
    WT_CACHE *cache;
    uint64_t bytes_max, bytes_updates;

    cache = S2C(session)->cache;

    /*
     * Avoid division by zero if the cache size has not yet been set in a shared cache.
     */
    bytes_max = S2C(session)->cache_size + 1;
    bytes_updates = __wt_cache_bytes_updates(cache);

    if (pct_fullp != NULL)
        *pct_fullp = (100.0 * bytes_updates) / bytes_max;

    return (bytes_updates > (uint64_t)(cache->eviction_updates_trigger * bytes_max) / 100);
}

/*
 * __wt_btree_dominating_cache --
 *     Return if a single btree is occupying at least half of any of our target's cache usage.
 */
static WT_INLINE bool
__wt_btree_dominating_cache(WT_SESSION_IMPL *session, WT_BTREE *btree)
{
    WT_CACHE *cache;
    uint64_t bytes_dirty;
    uint64_t bytes_max;

    cache = S2C(session)->cache;
    bytes_max = S2C(session)->cache_size + 1;

    if (__wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&btree->bytes_inmem)) >
      (uint64_t)(0.5 * cache->eviction_target * bytes_max) / 100)
        return (true);

    bytes_dirty =
      __wt_atomic_load64(&btree->bytes_dirty_intl) + __wt_atomic_load64(&btree->bytes_dirty_leaf);
    if (__wt_cache_bytes_plus_overhead(cache, bytes_dirty) >
      (uint64_t)(0.5 * cache->eviction_dirty_target * bytes_max) / 100)
        return (true);
    if (__wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&btree->bytes_updates)) >
      (uint64_t)(0.5 * cache->eviction_updates_target * bytes_max) / 100)
        return (true);

    return (false);
}

/*
 * __wt_eviction_needed --
 *     Return if an application thread should do eviction, and the cache full percentage as a
 *     side-effect.
 */
static WT_INLINE bool
__wt_eviction_needed(WT_SESSION_IMPL *session, bool busy, bool readonly, double *pct_fullp)
{
    WT_CACHE *cache;
    double pct_dirty, pct_full, pct_updates;
    bool clean_needed, dirty_needed, updates_needed;

    cache = S2C(session)->cache;

    /*
     * If the connection is closing we do not need eviction from an application thread. The eviction
     * subsystem is already closed.
     */
    if (F_ISSET(S2C(session), WT_CONN_CLOSING))
        return (false);

    clean_needed = __wt_eviction_clean_needed(session, &pct_full);
    if (readonly) {
        dirty_needed = updates_needed = false;
        pct_dirty = pct_updates = 0.0;
    } else {
        dirty_needed = __wt_eviction_dirty_needed(session, &pct_dirty);
        updates_needed = __wt_eviction_updates_needed(session, &pct_updates);
    }

    /*
     * Calculate the cache full percentage; anything over the trigger means we involve the
     * application thread.
     */
    if (pct_fullp != NULL)
        *pct_fullp = WT_MAX(0.0,
          100.0 -
            WT_MIN(
              WT_MIN(cache->eviction_trigger - pct_full, cache->eviction_dirty_trigger - pct_dirty),
              cache->eviction_updates_trigger - pct_updates));

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
 * __wt_cache_full --
 *     Return if the cache is at (or over) capacity.
 */
static WT_INLINE bool
__wt_cache_full(WT_SESSION_IMPL *session)
{
    WT_CACHE *cache;
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);
    cache = conn->cache;

    return (__wt_cache_bytes_inuse(cache) >= conn->cache_size);
}

/*
 * __wt_cache_hs_dirty --
 *     Return if a major portion of the cache is dirty due to history store content.
 */
static WT_INLINE bool
__wt_cache_hs_dirty(WT_SESSION_IMPL *session)
{
    WT_CACHE *cache;
    WT_CONNECTION_IMPL *conn;
    uint64_t bytes_max;
    conn = S2C(session);
    cache = conn->cache;
    bytes_max = conn->cache_size;

    return (__wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&cache->bytes_hs_dirty)) >=
      ((uint64_t)(cache->eviction_dirty_trigger * bytes_max) / 100));
}

/*
 * __evict_is_session_cache_trigger_tolerant --
 *     Check if the session is cache tolerant for the configured trigger values. If tolerant,
 *     session will not perform eviction.
 *
 * Cache tolerance is cache's ability to let application threads perform workload operations above
 *     trigger levels. Cache tolerance is applicable only for dirty_trigger and updates_trigger.
 *     Cache tolerance is expressed in % with respect to the trigger level. If cache tolerance is
 *     configured as 20%, and dirty trigger is 20%, then application threads will be incrementally
 *     involve in eviction as dirty content increases from 20% - 24%.
 *
 * All the application threads will perform eviction only when the dirty content reaches 24% of
 *     cache.
 *
 * Based on the % of cache usage above trigger level with respect to tolerance level, incrementally
 *     involve the app threads for eviction.
 *
 * Usage between 00% - 20% --> No application threads involve in eviction.
 *
 * Usage between 20% - 40% --> 20% of application threads involve in eviction.
 *
 * Usage between 40% - 60% --> 40% of application threads involve in eviction.
 *
 * Usage between 60% - 80% --> 60% of application threads involve in eviction.
 *
 * Usage between 80% - 100% --> 80% of application threads involve in eviction.
 *
 * Usage >= 100%, above tolerance level --> involve all application threads.
 */
static WT_INLINE bool
__evict_is_session_cache_trigger_tolerant(WT_SESSION_IMPL *session, uint8_t cache_tolerance)
{
    double dirty_trigger, updates_trigger;
    uint64_t bytes_dirty, bytes_dirty_tolerance, bytes_over_dirty_trigger;
    uint64_t bytes_dirty_trigger, bytes_max, bytes_updates_trigger;
    uint64_t bytes_updates, bytes_updates_tolerance, bytes_over_updates_trigger;

    bytes_max = S2C(session)->cache_size + 1;
    bytes_dirty = __wt_cache_dirty_leaf_inuse(S2C(session)->cache);
    dirty_trigger = S2C(session)->cache->eviction_dirty_trigger;
    bytes_dirty_trigger = (uint64_t)(dirty_trigger * bytes_max) / 100;
    /*
     * bytes_dirty_tolerance = number of bytes over the dirty trigger based on configured
     * cache_tolerance
     */
    bytes_dirty_tolerance = (uint64_t)(bytes_dirty_trigger * cache_tolerance) / 100;

    if (bytes_dirty > bytes_dirty_trigger) {
        /* Dirty content is more than dirty trigger. */
        bytes_over_dirty_trigger = bytes_dirty - bytes_dirty_trigger;

        if (bytes_over_dirty_trigger > bytes_dirty_tolerance) {
            /* More than 100% of tolerance level. 100% of the app threads are non-tolerant. */
            return (false);
        } else if (bytes_over_dirty_trigger * 5 > bytes_dirty_tolerance * 4) {
            /* 80% - 100% of tolerance level. 80% of app threads are non-tolerant. */
            if ((session->id % 5) > 0)
                return (false);
        } else if (bytes_over_dirty_trigger * 5 > bytes_dirty_tolerance * 3) {
            /* 60% - 80% of tolerance level. 60% of app threads are non-tolerant. */
            if ((session->id % 5) > 1)
                return (false);
        } else if (bytes_over_dirty_trigger * 5 > bytes_dirty_tolerance * 2) {
            /* 40% - 60% of tolerance level. 40% of app threads are non-tolerant. */
            if ((session->id % 5) > 2)
                return (false);
        } else if (bytes_over_dirty_trigger * 5 > bytes_dirty_tolerance * 1) {
            /* 20% - 40% of tolerance level. 20% of app threads are non-tolerant. */
            if ((session->id % 5) > 3)
                return (false);
        }
    }

    updates_trigger = S2C(session)->cache->eviction_updates_trigger;
    bytes_updates = __wt_cache_bytes_updates(S2C(session)->cache);
    bytes_updates_trigger = (uint64_t)(updates_trigger * bytes_max) / 100;
    /*
     * bytes_updates_tolerance = number of bytes over the update trigger based on configured
     * cache_tolerance
     */
    bytes_updates_tolerance = (uint64_t)(bytes_updates_trigger * cache_tolerance) / 100;

    if (bytes_updates > bytes_updates_trigger) {
        /* Updates content is more than update trigger. */
        bytes_over_updates_trigger = bytes_dirty - bytes_dirty_trigger;

        if (bytes_over_updates_trigger > bytes_updates_tolerance) {
            /* More than 100% of tolerance level. 100% of the app threads are non-tolerant. */
            return (false);
        } else if (bytes_over_updates_trigger * 5 > bytes_updates_tolerance * 4) {
            /* 80% - 100% of tolerance level. 80% of app threads are non-tolerant. */
            if ((session->id % 5) > 0)
                return (false);
        } else if (bytes_over_updates_trigger * 5 > bytes_updates_tolerance * 3) {
            /* 60% - 80% of tolerance level. 60% of app threads are non-tolerant. */
            if ((session->id % 5) > 1)
                return (false);
        } else if (bytes_over_updates_trigger * 5 > bytes_updates_tolerance * 2) {
            /* 40% - 60% of tolerance level. 40% of app threads are non-tolerant. */
            if ((session->id % 5) > 2)
                return (false);
        } else if (bytes_over_updates_trigger * 5 > bytes_updates_tolerance * 1) {
            /* 20% - 40% of tolerance level. 20% of app threads are non-tolerant. */
            if ((session->id % 5) > 3)
                return (false);
        }
    }
    /* session is tolerant for both the dirty trigger and update trigger. */
    return (true);
}

/*
 * __wt_cache_eviction_check --
 *     Evict pages if the cache crosses its boundaries.
 */
static WT_INLINE int
__wt_cache_eviction_check(WT_SESSION_IMPL *session, bool busy, bool readonly, bool *didworkp)
{
    WT_BTREE *btree;
    WT_CACHE *cache;
    WT_CONNECTION_IMPL *conn;
    WT_TXN_GLOBAL *txn_global;
    WT_TXN_SHARED *txn_shared;
    double pct_full;

    conn = S2C(session);
    cache = conn->cache;

    if (didworkp != NULL)
        *didworkp = false;

    /* It is not safe to proceed if the eviction server threads aren't setup yet. */
    if (!__wt_atomic_loadbool(&conn->evict_server_running))
        return (0);

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
    txn_global = &conn->txn_global;
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
    if (F_ISSET(conn, WT_CONN_IN_MEMORY))
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
    if (!__wt_eviction_needed(session, busy, readonly, &pct_full))
        return (0);

    /*
     * If the caller is holding shared resources, only evict if the cache is at any of its eviction
     * targets.
     */
    if (busy && pct_full < 100.0)
        return (0);
    /*
     * If the cache tolerance is configured, check if the session can be tolerant. if tolerant,
     * don't involve in eviction.
     */
    if (pct_full <= cache->eviction_trigger) {
        uint8_t cache_tolerance =
          __wt_atomic_load8(&conn->cache->cache_eviction_controls.cache_tolerance_for_app_eviction);
        if ((cache_tolerance != 0) &&
          (__evict_is_session_cache_trigger_tolerant(session, cache_tolerance)))
            return (0);
    }

    /*
     * Some callers (those waiting for slow operations), will sleep if there was no cache work to
     * do. After this point, let them skip the sleep.
     */
    if (didworkp != NULL)
        *didworkp = true;

    return (__wt_cache_eviction_worker(session, busy, readonly));
}
