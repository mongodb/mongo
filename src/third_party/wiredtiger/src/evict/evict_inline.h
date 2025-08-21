/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/* !!!
 * __wt_evict_aggressive --
 *     Check whether eviction is unable to make any progress for some amount of time.
 *
 *     As eviction continues to struggle, let the caller know that eviction has become inefficient
 *     (or made no progress). This helps determine if eviction strategies need to be more
 *     forceful due to ongoing inefficiencies. Additionally, it serves as a useful indicator of
 *     the health of the eviction process which callers can use to inform their behavior.
 */
static WT_INLINE bool
__wt_evict_aggressive(WT_SESSION_IMPL *session)
{
    return (
      __wt_atomic_load32(&S2C(session)->evict->evict_aggressive_score) >= WT_EVICT_SCORE_CUTOFF);
}

/* !!!
 * __wt_evict_cache_stuck --
 *     Check whether eviction has remained inefficient (or made no progress) for a significant
 *     period and that the cache has crossed the trigger thresholds even after significant
 *     efforts towards forceful eviction.
 *
 *     This function represents a more severe state compared to aggressive eviction and servers as a
 *     useful indicator of eviction's health, based on which callers may make certain choices to
 *     reduce cache pressure.
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

/* !!!
 * __wt_evict_page_is_soon_or_wont_need --
 *     Check whether a page is a candidate for forced eviction.
 *
 *     Pages marked with lower eviction state (read generation) including `WT_READGEN_EVICT_SOON`
 *     or `WT_READGEN_WONT_NEED` have precedence to be immediately removed from the cache.
 *
 *     At present, this function is called once during the decision of whether an application thread
 *     should perform forced eviction or urgently queue the page for eviction.
 *
 *     Input parameter:
 *       `page`: The page to be checked if it is subject to forced eviction.
 *
 *     Return `true` if the page should be forcefully evicted.
 */
static WT_INLINE bool
__wt_evict_page_is_soon_or_wont_need(WT_PAGE *page)
{
    return (__wti_evict_readgen_is_soon_or_wont_need(&page->read_gen));
}

/* !!!
 * __wt_evict_page_is_soon --
 *     Check whether a page is marked with the `WT_READGEN_EVICT_SOON` state, indicating that
 *     it should be evicted as soon as possible.
 *
 *     Currently, this function is called once when deciding whether to unpin the cursor to
 *     facilitate eviction. The `__wt_evict_page_is_soon_or_wont_need` function is not used in this
 *     context because only the `WT_READGEN_EVICT_SOON` state is relevant here (not the
 *     `WT_READGEN_WONT_NEED`).
 *
 *     Input parameter:
 *       `page`: The page to be checked for the evict soon state.
 *
 *     Return `true` if the page is marked to be evicted soon.
 */
static WT_INLINE bool
__wt_evict_page_is_soon(WT_PAGE *page)
{
    return (__wt_atomic_load64(&page->read_gen) == WT_READGEN_EVICT_SOON);
}

/* !!!
 * __wt_evict_page_soon --
 *     Mark the page to be evicted as soon as possible by setting the `WT_READGEN_EVICT_SOON`
 *     flag.
 *
 *     Once this flag is set, eviction threads aggressively prioritize evicting such pages
 *     by putting them in the urgent queue for immediate eviction. Furthermore, application
 *     threads that encounter these pages will either forcefully evict them or queue them
 *     for urgent eviction.
 *
 *     This function allows its callers to evict empty internal pages, pages exceeding a
 *     certain size, obsolete pages, pages with long skip list/update chains, among
 *     other similar cases.
 *
 *     Input parameter:
 *       `ref`: The reference to the page to be marked for soon eviction.
 */
static WT_INLINE void
__wt_evict_page_soon(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_UNUSED(session);

    __wt_atomic_store64(&ref->page->read_gen, WT_READGEN_EVICT_SOON);
}

/* !!!
 * __wt_evict_page_first_dirty --
 *     Update a page's eviction state (read generation) when a page transitions from clean to
 *     dirty.
 *
 *     It is called every time a page transitions from clean to dirty for the first time in memory.
 *
 *     Input parameter:
 *       `page`: The page whose eviction state is being updated.
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

/* !!!
 * __wt_evict_touch_page --
 *     Update a page's eviction state (read generation) when it is accessed.
 *
 *     A page that is recently read will have a higher read generation, indicating that it is less
 *     likely to be evicted. This mechanism helps eviction to prioritize the order in which pages
 *     are evicted.
 *
 *     This function is called every time a page is touched in the cache.
 *
 *     Input parameters:
 *       (1) `page`: The page whose eviction state is being updated.
 *       (2) `internal_only`: A flag indicating whether the operation is internal. If true, the read
 *            generation is not updated, as internal operations (such as compaction or eviction)
 *            should not affect the page's eviction priority.
 *       (3) `wont_need`: A flag indicating that the page will not be needed in the future. If true,
 *            the page is marked for forced eviction.
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

/* !!!
 * __wt_evict_page_init --
 *     Initialize the page's eviction state (read generation) for a newly created page in memory.
 *     Even if the page is evicted and later reallocated, this function will be called to reset
 *     the eviction state. This initialization is essential as it sets the `read_gen` value, which
 *     eviction uses to determine the priority of pages for eviction.
 *
 *     It is called only once when the page is first allocated in memory and should not be called
 *     again for that page.
 *
 *     Input parameter:
 *       `page`: The page for which to initialize the read generation.
 */
static WT_INLINE void
__wt_evict_page_init(WT_PAGE *page)
{
    __wt_atomic_store64(&page->read_gen, WT_READGEN_NOTSET);
}

/* !!!
 * __wt_evict_inherit_page_state --
 *     Initialize the read generation on the new page using the read generation of the original
 *     page, unless this was a forced eviction, in which case we leave the new page with the
 *     default initialization.
 *
 *     It is called when creating a new page from an existing page, for example during split.
 *
 *     Input parameters:
 *       (1) `orig_page`: The page from which to inherit the read generation.
 *       (2) `new_page`: The page for which to set the read generation.
 */
static WT_INLINE void
__wt_evict_inherit_page_state(WT_PAGE *orig_page, WT_PAGE *new_page)
{
    uint64_t orig_read_gen;

    WT_READ_ONCE(orig_read_gen, orig_page->read_gen);

    if (!__wti_evict_readgen_is_soon_or_wont_need(&orig_read_gen))
        __wt_atomic_store64(&new_page->read_gen, orig_read_gen);
}

/* !!!
 * __wt_evict_page_cache_bytes_decr --
 *     Decrement the in-memory byte count for the cache, B-tree, and page to reflect the eviction
 *     of a page.
 *
 *     It is called once each time a page is evicted from memory.
 *
 *     Input parameter:
 *       `page`: The page being evicted, for which byte counts are decremented.
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
        } else {
            __wt_cache_decr_check_uint64(
              session, &btree->bytes_dirty_leaf, modify->bytes_dirty, "WT_BTREE.bytes_dirty_leaf");
            __wt_cache_decr_check_uint64(
              session, &cache->bytes_dirty_leaf, modify->bytes_dirty, "WT_CACHE.bytes_dirty_leaf");
            __wt_cache_decr_check_uint64(session, &btree->bytes_delta_updates,
              modify->bytes_delta_updates, "WT_BTREE.bytes_delta_updates");
            __wt_cache_decr_check_uint64(session, &cache->bytes_delta_updates,
              modify->bytes_delta_updates, "WT_CACHE.bytes_delta_updates");
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

/* !!!
 * __wt_evict_clean_pressure --
 *     Check whether the cache is approaching or has surpassed its eviction trigger thresholds,
 *     indicating that application threads will soon be required to assist with eviction.
 *
 *     At present, this function is primarily called by the prefetch thread to determine whether it
 *     should avoid prefetching pages, as application threads may soon be involved in eviction.
 *
 *     Return `true` if the cache is nearing the eviction trigger thresholds.
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

/* !!!
 * __wt_evict_clean_needed --
 *     Check whether the configured eviction trigger threshold for the total volume of data in the
 *     cache has been reached. Once this threshold is met, application threads are signaled to
 *     assist with eviction. The eviction trigger threshold is configurable, and defined in
 *     `api_data.py`.
 *
 *     This function is called by the eviction server to determine the cache's current state and to
 *     set the internal flags accordingly.
 *
 *     Input parameter:
 *       `pct_full`: A pointer to store the percentage of cache used, if not NULL.
 *
 *     Return `true` if the cache usage exceeds the eviction trigger threshold.
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

/* !!!
 * __wt_evict_dirty_needed --
 *     Check whether the configured eviction dirty trigger threshold for the total volume
 *     of dirty data in the cache has been reached. Once this threshold is met, application threads
 *     are signaled to assist with the eviction of dirty pages. The eviction dirty trigger threshold
 *     is configurable, and defined in `api_data.py`.
 *
 *     This function is called by the eviction server to determine the cache's current
 *     state and to set the internal flags accordingly.
 *
 *     Input parameter:
 *       `pct_full`: A pointer to store the percentage of the cache used by the dirty leaf pages.
 *
 *     Return `true` if the cache usage exceeds the eviction dirty trigger threshold.
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

/* !!!
 * __wti_evict_updates_needed --
 *     Check whether the configured eviction update trigger threshold for the total volume of
 *     updates in the cache has been reached. Once this threshold is met, application threads are
 *     signaled to assist with the eviction of pages with updates. The eviction update trigger
 *     threshold is configurable, and defined in `api_data.py`.
 *
 *     This function is called by the eviction server to determine the cache's current
 *     state and to set the internal flags accordingly.
 *
 *     Input parameter:
 *       `pct_full`: A pointer to store the percentage of the cache used by updates.
 *
 *     Returns `true` if the cache usage exceeds the eviction update trigger threshold.
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

/* !!!
 * __wt_evict_needed --
 *     Check whether the configured clean/dirty/update eviction trigger thresholds for the cache
 *     have been reached. Once any of these thresholds are met, application threads are signaled
 *     to assist with the eviction of pages.
 *
 *     This function is called to determine whether cache is under pressure or application thread
 *     eviction is required.
 *
 *     Input parameters:
 *       (1) `busy`: A flag indicating if the session is actively pinning resources, in which
 *            case dirty trigger is ignored.
 *       (2) `readonly`: A flag indicating if the session is read-only, in which case dirty and
 *            update triggers are ignored.
 *       (3) `pct_full`: A pointer to store the calculated cache full percentage, if not NULL.
 *
 *     Return `true` if the cache usage exceeds any of the clean/dirty/update eviction trigger
 *     thresholds.
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
    if (F_ISSET_ATOMIC_32(S2C(session), WT_CONN_CLOSING))
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

/* !!!
 * __wt_evict_favor_clearing_dirty_cache --
 *    !!! Use this function with caution as it will significantly impact eviction behavior. !!!
 *
 *    Adjust eviction settings (`dirty_target` and `dirty_trigger`) to aggressively remove dirty
 *    bytes from the cache.
 *
 *    It should only be called once during `WT_CONNECTION::close`.
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
 * __evict_check_user_ok_with_eviction --
 *     Check if the user wants to abort eviction in this session. #private
 */
static bool
__evict_check_user_ok_with_eviction(WT_SESSION_IMPL *session, bool interruptible)
{
    /* Ask the user only if eviction is optional and it's a user session. */
    if (!interruptible || F_ISSET(session, WT_SESSION_INTERNAL))
        return (true);

    WT_EVENT_HANDLER *event_handler = session->event_handler;
    if (event_handler->handle_general != NULL &&
      event_handler->handle_general(
        event_handler, &S2C(session)->iface, &session->iface, WT_EVENT_EVICTION, NULL) != 0) {
        WT_STAT_CONN_INCR(session, eviction_interupted_by_app);
        return (false);
    }

    return (true);
}

/* !!!
 * __wt_evict_app_assist_worker_check --
 *     Check if eviction trigger thresholds have reached to determine whether application threads
 *     should assist eviction worker threads with eviction of pages from the queues.
 *
 *     Input parameters:
 *       (1) `busy`: A flag indicating if the session is actively pinning resources, in which
 *            case eviction work should be limited.
 *       (2) `readonly`: A flag indicating if the session is read-only, in which case dirty and
 *          update triggers are ignored.
 *       (3) `interruptible`: A flag indicating if the user is allowed to interrupt eviction.
 *       (4) `didworkp`: A pointer to indicate whether eviction work was done (optional).
 *
 *     Return an error code from `__wti_evict_app_assist_worker` if it is unable to perform
 *     meaningful work (eviction cache stuck).
 */
static WT_INLINE int
__wt_evict_app_assist_worker_check(
  WT_SESSION_IMPL *session, bool busy, bool readonly, bool interruptible, bool *didworkp)
{
    if (didworkp != NULL)
        *didworkp = false;

    /* It is not safe to proceed if the eviction server threads aren't setup yet. */
    WT_CONNECTION_IMPL *conn = S2C(session);
    if (!__wt_atomic_loadbool(&conn->evict_server_running))
        return (0);

    /* Eviction causes reconciliation. So don't evict if we can't reconcile */
    if (F_ISSET(session, WT_SESSION_NO_RECONCILE))
        return (0);

    /* If the transaction is prepared don't evict. */
    if (F_ISSET(session->txn, WT_TXN_PREPARE))
        return (0);

    /* FIXME-WT-12905: Pre-fetch threads are not allowed to be pulled into eviction. */
    if (F_ISSET(session, WT_SESSION_PREFETCH_THREAD))
        return (0);

    /*
     * If the transaction is a checkpoint cursor transaction, don't try to evict. Because eviction
     * keeps the current transaction snapshot, and the snapshot in a checkpoint cursor transaction
     * can be (and likely is) very old, we won't be able to see anything current to evict and won't
     * be able to accomplish anything useful.
     */
    if (F_ISSET(session->txn, WT_TXN_IS_CHECKPOINT))
        return (0);

    /* Setting cache_max_wait_us to 1 effectively means "disable eviction when possible" */
    uint64_t cache_max_wait_us =
      session->cache_max_wait_us != 0 ? session->cache_max_wait_us : conn->evict->cache_max_wait_us;
    if (cache_max_wait_us == 1)
        return (0);

    /*
     * If the current transaction is keeping the oldest ID pinned, it is in the middle of an
     * operation. This may prevent the oldest ID from moving forward, leading to deadlock, so only
     * evict what we can. Otherwise, we are at a transaction boundary and we can work harder to make
     * sure there is free space in the cache.
     */
    WT_TXN_GLOBAL *txn_global = &conn->txn_global;
    WT_TXN_SHARED *txn_shared = WT_SESSION_TXN_SHARED(session);
    busy = busy || __wt_atomic_loadv64(&txn_shared->id) != WT_TXN_NONE ||
      session->hazards.num_active > 0 ||
      (__wt_atomic_loadv64(&txn_shared->pinned_id) != WT_TXN_NONE &&
        __wt_atomic_loadv64(&txn_global->current) != __wt_atomic_loadv64(&txn_global->oldest_id));

    /*
     * Don't block the thread for eviction when holding the handle list, schema or table locks
     * (which can block checkpoints and eviction), or if the "ignore cache size" flag is set. Some
     * internal threads such as the sweep server and background compact will set the "ignore cache
     * size" flag for this reason. Sessions can also set this flag using the "ignore_cache_size"
     * configuration.
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
    WT_BTREE *btree = S2BT_SAFE(session);
    if (btree != NULL && (F_ISSET(btree, WT_BTREE_NO_EVICT) || WT_IS_METADATA(session->dhandle)))
        return (0);

    /* Check if eviction is needed. */
    double pct_full;
    if (!__wt_evict_needed(session, busy, readonly, &pct_full))
        return (0);

    /*
     * If the caller is holding shared resources, only evict if the cache is at any of its eviction
     * targets.
     */
    if (busy && pct_full < 100.0)
        return (0);

    /* Last check if application wants to prevent the thread from evicting. */
    if (!__evict_check_user_ok_with_eviction(session, interruptible))
        return (0);

    /*
     * Some callers (those waiting for slow operations), will sleep if there was no cache work to
     * do. After this point, let them skip the sleep.
     */
    if (didworkp != NULL)
        *didworkp = true;

    return (__wti_evict_app_assist_worker(session, busy, readonly, interruptible));
}

/*
 * __wt_evict_clear_npos --
 *     Clear saved eviction walk position.
 */
static WT_INLINE void
__wt_evict_clear_npos(WT_BTREE *btree)
{
    btree->evict_pos = WT_NPOS_INVALID;
    btree->evict_saved_ref_check = 0;
}
